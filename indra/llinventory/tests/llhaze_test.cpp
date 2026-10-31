/** @file llhaze_test.cpp
 * Environment precedence, transition continuity and height transport invariants.
 */
#include "linden_common.h"
#include "llhaze.h"
#include "../test/lltut.h"
#include <limits>

namespace tut
{
struct haze_data {};
using haze_test = test_group<haze_data>;
using haze_object = haze_test::object;
haze_test haze_group("llhaze");

template<> template<> void haze_object::test<1>()
{
    set_test_name("independent per-field precedence including zero and Automatic");
    for(int start=0;start<3;++start)
        for(int cap=0;cap<3;++cap)
        {
            LLHaze::Saved saved;
            saved.startDistance={start==1,0.};
            saved.maxOpacity={cap==1,0.};
            auto resolved=LLHaze::overrideLocal(LLHaze::resolveEndpoint({},saved),start==2 ? 0. : -1.,cap==2 ? -0. : -1.);
            ensure("start source",resolved.startDistance.source==(start==2 ? LLHaze::Source::User : start==1 ? LLHaze::Source::Sky : LLHaze::Source::Derived));
            ensure("opacity source",resolved.maxOpacity.source==(cap==2 ? LLHaze::Source::User : cap==1 ? LLHaze::Source::Sky : LLHaze::Source::Derived));
            if(start) ensure_equals("zero start",resolved.startDistance.value,0.);
            if(cap) ensure_equals("zero cap",resolved.maxOpacity.value,0.);
        }
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const auto malformed=LLHaze::overrideLocal(LLHaze::resolveEndpoint({},{{true,nan},{true,-2.}}),nan,-2.);
    ensure("malformed start derives",malformed.startDistance.source==LLHaze::Source::Derived);
    ensure("malformed opacity derives",malformed.maxOpacity.source==LLHaze::Source::Derived);
}

template<> template<> void haze_object::test<2>()
{
    set_test_name("derive calibration and hostile input sanitation");
    auto day=LLHaze::derive({});
    ensure("nominal density",std::abs(day.sigma0-.000056)<1.e-12);
    ensure("nominal scale height",std::abs(day.scaleHeight-32.1)<1.e-12);
    ensure("nominal day start",std::abs(day.automatic.startDistance.value-7.3788)<.01);
    LLHaze::Inputs inputs;
    inputs.illuminationRatio=0.;
    const auto night=LLHaze::derive(inputs);
    ensure("night starts later",night.automatic.startDistance.value>day.automatic.startDistance.value);
    ensure("night cap lower",night.automatic.maxOpacity.value<day.automatic.maxOpacity.value);
    inputs={1.e300,1.e300,1.e300,1.e300,std::numeric_limits<double>::infinity()};
    const auto huge=LLHaze::derive(inputs);
    ensure_equals("bounded density",huge.sigma0,.1);
    ensure_equals("bounded H",huge.scaleHeight,200.);
    ensure("finite guardrails",std::isfinite(huge.automatic.startDistance.value) && std::isfinite(huge.automatic.maxOpacity.value));
}

template<> template<> void haze_object::test<3>()
{
    set_test_name("endpoint resolution before blend and bounded interruption snapshots");
    LLHaze::Transition transition;
    transition.from.saved.startDistance={true,0.};
    transition.to.saved.maxOpacity={true,.95};
    transition.to.inputs.illuminationRatio=0.;
    transition.weight=.37;
    const auto from=LLHaze::resolveEndpoint(transition.from),to=LLHaze::resolveEndpoint(transition.to);
    const auto current=LLHaze::resolveTransition(transition);
    ensure("endpoint start blend",std::abs(current.startDistance.value-(from.startDistance.value*.63+to.startDistance.value*.37))<1.e-12);
    ensure("endpoint cap blend",std::abs(current.maxOpacity.value-(from.maxOpacity.value*.63+to.maxOpacity.value*.37))<1.e-12);
    for(int i=0;i<10000;++i)
    {
        transition.from=LLHaze::freeze(transition);
        transition.weight=0.;
        const auto snapshot=LLHaze::resolveTransition(transition);
        ensure_equals("continuous interruption start",snapshot.startDistance.value,current.startDistance.value);
        ensure_equals("continuous interruption cap",snapshot.maxOpacity.value,current.maxOpacity.value);
    }
    ensure("bounded value metadata",sizeof(transition)<512);
    auto overridden=LLHaze::overrideLocal(current,20.,-1.);
    ensure_equals("override after blend",overridden.startDistance.value,20.);
    ensure_equals("other field unaffected",overridden.maxOpacity.value,current.maxOpacity.value);
}

template<> template<> void haze_object::test<4>()
{
    set_test_name("height integral base and water crossings and nearly horizontal rays");
    using LLHaze::heightOpticalDepth;
    const double sigma=.01,H=32.;
    ensure_equals("zero medium",heightOpticalDepth(0.,H,0.,0.,2.,0.,0.,128.),0.);
    ensure("horizontal integral",std::abs(heightOpticalDepth(sigma,H,0.,0.,2.,0.,0.,128.)-sigma*128.*std::exp(-2./H))<1.e-12);
    ensure_equals("underwater identity",heightOpticalDepth(sigma,H,0.,0.,-2.,0.,0.,128.),0.);
    ensure_equals("start beyond endpoint",heightOpticalDepth(sigma,H,0.,0.,2.,0.,0.,128.,128.),0.);
    const double crossing=heightOpticalDepth(sigma,H,10.,0.,-10.,1.,0.,40.);
    const double analytic=sigma*(10.+H*(1.-std::exp(-20./H)));
    ensure("base crossing split",std::abs(crossing-analytic)<1.e-12);
    const double down=heightOpticalDepth(sigma,H,10.,0.,30.,-1.,0.,40.);
    ensure("downhill matches uphill",std::abs(crossing-down)<1.e-12);
    ensure("tiny slope continuous",std::abs(heightOpticalDepth(sigma,H,0.,0.,2.,1.e-20,0.,128.)-heightOpticalDepth(sigma,H,0.,0.,2.,0.,0.,128.))<1.e-12);
    ensure("extreme downhill finite",std::isfinite(heightOpticalDepth(sigma,H,0.,0.,1.e6,-1.,0.,1.e6)));
}

template<> template<> void haze_object::test<5>()
{
    set_test_name("height-only cap and homogeneous source conservation");
    for(double opacity : {0.,.4,.95})
    {
        const double cap=LLHaze::opacityOpticalDepth(opacity);
        double used=0.;
        for(int i=0;i<100;++i) LLHaze::consumeHeight(.17,cap,used);
        ensure("cap respected",used<=cap+1.e-14);
        ensure("opacity cap",std::abs(1.-std::exp(-used)-opacity)<1.e-12);
        ensure_equals("no height source after cap",LLHaze::consumeHeight(.3,cap,used),0.);
    }
    const double source=.2,extinction=.03,distance=128.;
    const double full=LLHaze::slabSource(source,extinction,distance);
    double total=0.,transmittance=1.;
    for(int i=0;i<32;++i)
    {
        total+=transmittance*LLHaze::slabSource(source,extinction,distance/32.);
        transmittance*=std::exp(-extinction*distance/32.);
    }
    ensure("subdivision invariance",std::abs(total-full)<1.e-12);
    ensure_equals("zero-coefficient limit",LLHaze::slabSource(source,0.,distance),source*distance);
}
template<> template<> void haze_object::test<6>()
{
    set_test_name("reserved start boundary support and previous-map inverse");
    for(int count : {32,48,64,96}) for(float range : {64.f,128.f,512.f})
    for(float factor : {1.f,.8f,.5f,.1f})
    for(float start : {0.f,1.e-6f,16.f,range,range/factor,range/factor+1.f})
    {
        const float n=range/64.f;
        float previous=0.f;
        for(int boundary=0;boundary<=count;++boundary)
        {
            const float u=static_cast<float>(boundary)/count;
            const float depth=LLHaze::sliceDepth(u,factor,start,range,n);
            ensure("finite monotone boundaries",std::isfinite(depth) && depth>=previous-1.e-4f);
            if(depth>0.f && depth<range)
            {
                const float inverse=LLHaze::sliceCoordinate(depth,factor,start,range,n);
                ensure("inverse roundtrip",std::abs(inverse-u)<1.e-5f);
                ensure("far-boundary texel index",std::abs(inverse*count-1.f-(boundary-1.f))<.002f);
            }
            previous=depth;
        }
        ensure_equals("origin identity",LLHaze::sliceDepth(0.f,factor,start,range,n),0.f);
        ensure_equals("fixed far endpoint",LLHaze::sliceDepth(1.f,factor,start,range,n),range);
        ensure_equals("reserved boundary",LLHaze::sliceDepth(.25f,factor,start,range,n),std::clamp(start*factor,0.f,range));
        for(float distance : {0.f,start*.5f,std::nextafter(start,0.f),start})
            ensure("no post-start filter support",LLHaze::prefixIndex(distance,factor,start,range,n,static_cast<float>(count))<=count*.25f-1.f);
        // Histories invert PREVIOUS numerical map after reconstructing world position
        // through the current map; an animated start does not retain its old index.
        const float worldDistance=LLHaze::sliceDepth(.6f,factor,start,range,n)/factor;
        const float oldStart=start+.1f;
        const float oldU=LLHaze::sliceCoordinate(worldDistance*factor,factor,oldStart,range,n);
        ensure("previous mapping finite",std::isfinite(oldU));
        const float recovered=LLHaze::sliceDepth(oldU,factor,oldStart,range,n);
        ensure("previous map recovers world position",std::abs(recovered-worldDistance*factor)<range*2.e-5f);
    }
}

template<> template<> void haze_object::test<7>()
{
    set_test_name("trilinear support preserves exact start and independent RGB air/local opacity");
    constexpr int count=32;
    constexpr float start=16.f,range=128.f,n=2.f;
    const double cap=LLHaze::opacityOpticalDepth(.2);
    // Different ray factors model four XY neighbours, including frustum corners.
    // Each prefix stores its FAR boundary, rather than the cell centre.
    const float factors[4]={1.f,.8f,.5f,.1f};
    std::array<std::array<double,count>,4> height{},radiance{};
    std::array<std::array<std::array<double,3>,count>,4> air{},combined{};
    for(int column=0;column<4;++column)
        for(int z=0;z<count;++z)
        {
            const double distance=LLHaze::sliceDepth(float(z+1)/count,factors[column],start,range,n)/factors[column];
            const double tau=std::min(.01*std::max(distance-start,0.),cap);
            height[column][z]=tau;
            radiance[column][z]=.9*(-std::expm1(-tau));
            for(int c=0;c<3;++c)
            {
                // Local opacity remains before start, independently of RGB air.
                air[column][z][c]=std::exp(-(.001*(c+1)+.003)*distance);
                combined[column][z][c]=air[column][z][c]*std::exp(-tau);
            }
        }
    for(float factor : factors)
        for(float distance : {0.f,8.f,15.999f,std::nextafter(start,0.f),start,std::nextafter(start,32.f),16.001f,32.f,128.f})
        {
            const float q=LLHaze::prefixIndex(distance,factor,start,range,n,float(count));
            const int z0=std::clamp(int(std::floor(std::max(q,0.f))),0,count-1),z1=std::min(z0+1,count-1);
            const double wz=std::max(q,0.f)-z0;
            for(double x : {0.,.25,.5,.75,1.}) for(double y : {0.,.25,.5,.75,1.})
            {
                const double xy[4]={(1.-x)*(1.-y),x*(1.-y),(1.-x)*y,x*y};
                double tau=0.,light=0.;
                std::array<double,3> ta{},tc{};
                for(int column=0;column<4;++column) for(int tap=0;tap<2;++tap)
                {
                    const int z=tap ? z1 : z0;
                    const double weight=xy[column]*(tap ? wz : 1.-wz);
                    tau+=weight*height[column][z]; light+=weight*radiance[column][z];
                    for(int c=0;c<3;++c)
                    {
                        ta[c]+=weight*air[column][z][c];
                        tc[c]+=weight*combined[column][z][c];
                    }
                }
                const double originFraction=q<0.f ? std::clamp(distance*factor/LLHaze::sliceDepth(1.f/count,factor,start,range,n),0.f,1.f) : 1.;
                tau*=originFraction; light*=originFraction;
                for(int c=0;c<3;++c)
                {
                    ta[c]=1.+originFraction*(ta[c]-1.);
                    tc[c]=1.+originFraction*(tc[c]-1.);
                }
                ensure("finite bounded interpolated source",std::isfinite(light) && light>=0. && light<=.18+1.e-14);
                for(int c=0;c<3;++c) ensure("height cap keeps RGB air/local",tc[c]>=.8*ta[c]-1.e-14);
                if(distance<=start)
                {
                    ensure_equals("exact zero height support",tau,0.);
                    ensure_equals("exact zero height source",light,0.);
                    for(int c=0;c<3;++c)
                    {
                        ensure_equals("air/local retained before start",tc[c],ta[c]);
                        if(distance==0.f) ensure_equals("origin identity",tc[c],1.);
                        else ensure("nonzero local/air opacity before start",tc[c]<1.);
                    }
                }
            }
        }
    const double at32=std::exp(-.01*(32.-start));
    ensure("homogeneous endpoint reference",std::abs(at32-.8521437889662113)<1.e-14);
    ensure("source uses same extinction",std::abs(.9*(1.-at32)-.1330705899304098)<1.e-14);
}

}
