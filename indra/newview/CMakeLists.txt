# -*- cmake -*-

project(viewer)

# Clean-pipeline diagnostics master switch (indra/newview/llcleandebug.h).
# -DCLEAN_DEBUG=1 compiles in the heavy clean-pipeline diagnostics; 0 (and the
# header default when unset) builds the production viewer with no overhead.
if(DEFINED CLEAN_DEBUG)
  add_compile_definitions(CLEAN_DEBUG=${CLEAN_DEBUG})
endif()

# Developer test-control channel (sltestctl.h): a localhost, token-authenticated socket
# that drives login/logout/settings/camera/snapshots for autonomous testing. OFF by
# default and refused for release builds (CI passes a non-zero VIEWER_BUILD_ID).
option(SLIPSTREAM_TESTCTL "Compile in the developer test-control channel (never ship)" OFF)
if(SLIPSTREAM_TESTCTL)
  if(VIEWER_BUILD_ID AND NOT VIEWER_BUILD_ID STREQUAL "0")
    message(FATAL_ERROR "SLIPSTREAM_TESTCTL must not be enabled in a release build (VIEWER_BUILD_ID=${VIEWER_BUILD_ID})")
  endif()
  if(NOT LINUX)
    message(FATAL_ERROR "SLIPSTREAM_TESTCTL is Linux-only")
  endif()
  message(STATUS "SLIPSTREAM_TESTCTL=ON: developer test-control channel compiled in")
endif()

include(00-Common)
# DON'T move Linking.cmake to its place in the alphabetized list below: it
# sets variables on which the 3p .cmake files depend.
include(Linking)

include(Boost)
include(BuildPackagesInfo)
include(BuildVersion)
include(CMakeCopyIfDifferent)
include(CubemapToEquirectangularJS)
include(DragDrop)
include(EXPAT)
include(Hunspell)
include(JPEGEncoderBasic)
include(LLAppearance)
include(LLAudio)
include(LLCA)
include(LLCommon)
include(LLCoreHttp)
include(LLImage)
include(LLPhysicsExtensions)
include(LLPrimitive)
include(LLWindow)
include(NDOF)
include(NVAPI)
include(OPENAL)
include(OpenSSL)
include(PNG)
include(TemplateCheck)
include(ThreeJS)
include(Tracy)
include(UI)
include(ViewerMiscLibs)
include(SDL3)        # EXPERIMENTAL Vulkan: SDL_Vulkan_* on the viewer window
include(VulkanGfx)   # EXPERIMENTAL Vulkan: ll::vulkan (headers + loader)
include(VisualLeakDetector)
include(WebP)        # libwebp from source (#651, WebP chat image previews)
include(ZLIBNG)
include(LLPrimitive)

# The stub package is a source tarball with its own CMakeLists.txt, built into our tree.
add_subdirectory(${LLPHYSICSEXTENSIONS_SRC_DIR} llphysicsextensions)
cmake_policy( SET CMP0079 NEW)
target_link_libraries(llphysicsextensionsstub llcommon llmath)

# ReShade FX effect compiler (#543): vendored reshadefx static library (+ the slfxc
# developer smoke test, not built by default). See vkgfx/third_party/reshadefx/README.md.
add_subdirectory(vkgfx/third_party/reshadefx reshadefx)

set(viewer_SOURCE_FILES
    lutcube.cpp
    slassetblocklist.cpp
    slfloatersoundexplorer.cpp
    slfloaterweather.cpp
    slsoundlog.cpp
    slweather.cpp
    slweatheraudio.cpp
    slweatherregion.cpp
    slparcelspeakers.cpp
    slparcelspeakeraudio.cpp
    slmediaurlpolicy.cpp
    slavatarcomplexitytext.cpp
    slavataractions.cpp
    slchatbar.cpp
    slchatcommand.cpp
    slfloatergenerictext.cpp
    slfloaterparticleeditor.cpp
    slfloaterregiontracker.cpp
    slfloaterspeakersetup.cpp
    slpanelmusicticker.cpp
    slpanelradaralert.cpp
    slpanelstreaminfo.cpp
    slstreaminfo.cpp
    sltpscamera.cpp
    slspeedfov.cpp
    slmmbnav.cpp
    sltoolalign.cpp
    slunzip.cpp
    slviewermenu.cpp
    bdfloatercamera.cpp
    fscombatads.cpp
    fscombataim.cpp
    sscombatray.cpp
    fscommon.cpp
    fscontactsfriendsmenu.cpp
    fsdroptarget.cpp
    fskeywords.cpp
    fscombathitmarker.cpp      # <GMDX> kill feed
    gmdxminimapcompass.cpp     # <GMDX> mini-map compass (Show Whole Region)
    gmdxminimapimage.cpp       # <GMDX> mini-map Copy / Save Map Image
    gmdxminimapterrain.cpp     # <GMDX> mini-map terrain background
    gmdxregioncapture.cpp      # <GMDX> region map capture
    gmdxchatlog.cpp      # <GMDX> chat log
    gmdxcrashreport.cpp      # <GMDX> crash reports (Crashpad -> BugSplat)
    gmdxminimapmarks.cpp      # <GMDX> mini-map marks
    gmdxfloaterloadouts.cpp    # <GMDX> loadouts window and swap toast
    gmdxloadouts.cpp           # <GMDX> loadouts
    gmdxreplay.cpp             # <GMDX> instant replay
    gmdxreplayaudio.cpp        # <GMDX> instant replay: the viewer's own sound, Opus
    gmdxweaponsorter.cpp       # <GMDX> offers to sort new weapons into the loadout folders
    gmdxrezfx.cpp              # <GMDX> rez effects
    gmdxselectionoutline.cpp   # <GMDX> LL selection silhouettes
    fsfloaterhitmarker.cpp     # <GMDX> hit marker
    fsfloaterkillfeed.cpp      # <GMDX> kill feed
    fsfloateraddtocontactset.cpp
    fsfavoritegroups.cpp
    fsfloatercontacts.cpp
    fspanelcontactsets.cpp
    fsfloatercontactsetconfiguration.cpp
    fsfloatergrouptitles.cpp
    fsfloatergroupviewer.cpp # <GMDX>
    fsgrouptitleregionmgr.cpp
    fsfloaterradar.cpp
    fspanelface.cpp
    fspanelradar.cpp
    fsradar.cpp
    fsradarentry.cpp
    fsradarlistctrl.cpp
    fsradarmenu.cpp
    fsscrolllistctrl.cpp
    lggcontactsets.cpp
    fsfloaterposer.cpp
    fsjointpose.cpp
    fsmaniprotatejoint.cpp
    fsmaniptranslatejoint.cpp
    fsposeranimator.cpp
    fsposestate.cpp
    fsposingmotion.cpp
    slaoengine.cpp
    slaoobjectimport.cpp
    slaoset.cpp
    slaosets.cpp
    slbridge.cpp
    slscriptinfo.cpp
    sldatausage.cpp
    slcachemetrics.cpp
    slassetcachestore.cpp
    slassetmigrate.cpp
    slcachemigrate.cpp
    slcachestore.cpp
    slgputexmigrate.cpp
    slj2cmigrate.cpp
    sltexturetiers.cpp
    slcolorwheelctrl.cpp
    slcolorwheelmodel.cpp
    slcurveeditorctrl.cpp
    slcurvemodel.cpp
    sldaycyclelandmarks.cpp
    sldiscordpresence.cpp
    slfloaterao.cpp
    slfloaterfirstrun.cpp
    slfloatergesturesets.cpp
    slfloaterphotostudio.cpp
    slfloatershapeeditor.cpp
    slfloaterposecreator.cpp
    slfloaterposestudio.cpp
    slfloaterviewerimport.cpp
    slfxini.cpp
    slfxpresets.cpp
    slkeycommands.cpp
    slgesturesets.cpp
    slgradehistory.cpp
    slpanelgesturesetspulldown.cpp
    slpanelphotostudioeffects.cpp
    slpanelphotostudiosection.cpp
    slpanelshapeeditor.cpp
    slpanelpreferencebuilding.cpp
    slphotostudiodirectory.cpp
    slphotostudiodraw.cpp
    slphotostudioscopes.cpp
    slphotostudiosettings.cpp
    slposecreator.cpp
    slposedialctrl.cpp
    slposefigurectrl.cpp
    slposeik.cpp
    slposelookat.cpp
    slposemath.cpp
    slposestudio.cpp
    slposesync.cpp
    slprimdefaults.cpp
    slrevocation.cpp
    slrevocationupdater.cpp
    slriskysettings.cpp
    slsettingsmigration.cpp
    slscopedata.cpp
    slswim.cpp
    sltoolscenepicker.cpp
    slupdateapply.cpp
    slupdatechecker.cpp
    slupdater.cpp
    slupdateverify.cpp
    slviewerimportcore.cpp
    slviewerimportplan.cpp
    slviewerpresets.cpp
    slwhitebalancesolver.cpp
    gltfscenemanager.cpp
    gltf/asset.cpp
    gltf/accessor.cpp
    gltf/primitive.cpp
    gltf/animation.cpp
    gltf/llgltfloader.cpp
    fslslpreproc.cpp
    fslslpreprocviewer.cpp
    groupchatlistener.cpp
    llaccountingcostmanager.cpp
    llaisapi.cpp
    llagent.cpp
    llagentaccess.cpp
    llagentbenefits.cpp
    llagentcamera.cpp
    llagentdata.cpp
    llagenthandler.cpp
    llagentlanguage.cpp
    llagentlistener.cpp
    llagentpicksinfo.cpp
    llagentpilot.cpp
    llagentui.cpp
    llagentwearables.cpp
    llanimstatelabels.cpp
    llappcorehttp.cpp
    llappearancelistener.cpp
    llappearancemgr.cpp
    llappviewer.cpp
    llappviewerlistener.cpp
    llattachmentsmgr.cpp
    llaudiosourcevo.cpp
    llautoreplace.cpp
    llavataractions.cpp
    llavatariconloader.cpp
    llavatariconctrl.cpp
    llavatarlist.cpp
    llavatarlistitem.cpp
    llavatarrenderinfoaccountant.cpp
    llavatarrendernotifier.cpp
    llavatarpropertiesprocessor.cpp
    llblockedlistitem.cpp
    llblocklist.cpp
    llbox.cpp
    llbrowsernotification.cpp
    llbuycurrencyhtml.cpp
    llcallingcard.cpp
    llchannelmanager.cpp
    llchatbar.cpp
    llchathistory.cpp
    llchatimagecache.cpp
    llchatimagegif.cpp
    llchatimagewebp.cpp
    llchatimagepreview.cpp
    llchatitemscontainerctrl.cpp
    llchatmsgbox.cpp
    llcheatcodes.cpp
    llchiclet.cpp
    llchicletbar.cpp
    llchunkedpurge.cpp
    llclassifiedinfo.cpp
    llclassifieditem.cpp
    llcofwearables.cpp
    llcolorswatch.cpp
    llcommanddispatcherlistener.cpp
    llcommandhandler.cpp
    llcommandlineparser.cpp
    llcommunicationchannel.cpp
    llcompilequeue.cpp
    llconfirmationmanager.cpp
    llcontrolavatar.cpp
    llconversationlog.cpp
    llconversationloglist.cpp
    llconversationloglistitem.cpp
    llconversationmodel.cpp
    llconversationview.cpp
    llcullcamera.cpp
    llcurrencyuimanager.cpp
    llcylinder.cpp
    lldateutil.cpp
    lldebugmessagebox.cpp
    lldebugview.cpp
    lldeferredsounds.cpp
    lldelayedgestureerror.cpp
    llderenderlist.cpp
    lldirpicker.cpp
    lldonotdisturbnotificationstorage.cpp
    lldndbutton.cpp
    lldrawable.cpp
    lldrawpool.cpp
    llcleaninstancestore.cpp
    llcleanacoustics.cpp
    llcleancursorpick.cpp
    llcleanrigdecimate.cpp
    llcleanclusterbuild.cpp           # cluster LOD ingest: clusterize + DAG (#744)
    llcleanclustercache.cpp           # cluster LOD disk cache (#744)
    llcleanclustercachestore.cpp      # its segment-store backend (#715)
    llcleanrigshadowlod.cpp
    llcleanpartpick.cpp
    llcleanmaterialstore.cpp
    llcleanmeshasset.cpp
    llcleanparticlestore.cpp
    llcleananimstore.cpp
    llcleanskinstore.cpp
    llcleansysavatar.cpp
    llcleaningest.cpp
    llcrossingkeep.cpp
    llcrossingtrace.cpp
    llmemdiag.cpp
    llmemtrendprobes.cpp
    llcleangltfingest.cpp
    llcleanterrainsmooth.cpp
    llcleantexture.cpp
    lldroptarget.cpp
    lldynamictexture.cpp
    llemote.cpp
    llenvironment.cpp
    llestateinfomodel.cpp
    lleventnotifier.cpp
    lleventpoll.cpp
    llexpandabletextbox.cpp
    llexperiencelog.cpp
    llexternaleditor.cpp
    llface.cpp
    llfasttimerview.cpp
    llfavoritesbar.cpp
    llfeaturemanager.cpp
    llfetchedgltfmaterial.cpp
    llfilepicker.cpp
    llfilteredwearablelist.cpp
    llfirstuse.cpp
    llflexibleobject.cpp
    llfloater360capture.cpp
    llfloaterabout.cpp
    llfloateravatarwelcomepack.cpp
    llfloaterbvhpreview.cpp
    llfloateraddpaymentmethod.cpp
    llfloaterauction.cpp
    llfloaterautoreplacesettings.cpp
    llfloateravatarpicker.cpp
    llfloateravatarrendersettings.cpp
    llfloateravatartextures.cpp
    llfloaterbanduration.cpp
    llfloaterbeacons.cpp
    llfloaterbigpreview.cpp
    llfloaterblocked.cpp
    llfloaterbuildoptions.cpp
    llfloaterbulkpermission.cpp
    llfloaterbulkupload.cpp
    llfloaterbump.cpp
    llfloaterbuy.cpp
    llfloaterbuycontents.cpp
    llfloaterbuycurrency.cpp
    llfloaterbuycurrencyhtml.cpp
    llfloaterbuyland.cpp
    llfloatercamera.cpp
    llfloatercamerapresets.cpp
    llfloaterchangeitemthumbnail.cpp
    llfloaterchatmentionpicker.cpp
    llfloaterchatimage.cpp
    llfloaterchatvoicevolume.cpp
    llfloaterclassified.cpp
    llfloatercolorpicker.cpp
    llfloaterconversationlog.cpp
    llfloaterconversationpreview.cpp
    llfloatercreatelandmark.cpp
    llfloaterdeleteprefpreset.cpp
    llfloaterdestinations.cpp
    llfloaterdirectory.cpp
    llfloaterdisplayname.cpp
    llfloatereditenvironmentbase.cpp
    llfloatereditextdaycycle.cpp
    llfloateremojipicker.cpp
    llfloaterenvironmentadjust.cpp
    llfloaterevent.cpp
    llfloaterexperiencepicker.cpp
    llfloaterexperienceprofile.cpp
    llfloaterexperiences.cpp
    llfloaterfixedenvironment.cpp
    llfloaterfonttest.cpp
    llfloaterforgetuser.cpp
    llfloatergesture.cpp
    llfloatergltfasseteditor.cpp
    llfloatergltfexport.cpp
    llfloatergodtools.cpp
    llfloatergotoline.cpp
    llfloatergridstatus.cpp
    llfloatergroupbulkban.cpp
    llfloatergroupinvite.cpp
    llfloatergroupprofile.cpp
    llfloatergroups.cpp
    llfloaterhandler.cpp
    llfloaterhelpbrowser.cpp
    llfloaterhoverheight.cpp
    llfloaterhowto.cpp
    llfloaterhud.cpp
    llfloaterimagepreview.cpp
    llfloaterimsessiontab.cpp
    llfloaterimsession.cpp
    llfloaterimcontainer.cpp
    llfloaterinspect.cpp
    llfloaterinventorysettings.cpp
    llfloaterinventorythumbnailshelper.cpp
    llfloaterjoystick.cpp
    llfloaterlagmeter.cpp
    llfloaterland.cpp
    llfloaterlandholdings.cpp
    llfloaterlinkreplace.cpp
    llfloaterloadprefpreset.cpp
    llfloaterlocalassets.cpp
    llfloatermarketplacelistings.cpp
    llfloatermap.cpp
    llfloatermediasettings.cpp
    llfloatermemleak.cpp
    llfloatermeshbulkupload.cpp
    llfloatermodelpreview.cpp
    llfloatermodeluploadbase.cpp
    llfloatermyscripts.cpp
    llfloatermyenvironment.cpp
    llfloaternamedesc.cpp
    llfloaternewfeaturenotification.cpp
    llfloaternotificationsconsole.cpp
    llfloaternotificationstabbed.cpp
    llfloaterobjectweights.cpp
    llfloatertoolstexturedock.cpp
    llfloateropenobject.cpp
    llfloatersimplesnapshot.cpp
    llfloaterpathfindingcharacters.cpp
    llfloaterpathfindingconsole.cpp
    llfloaterpathfindinglinksets.cpp
    llfloaterpathfindingobjects.cpp
    llfloaterpay.cpp
    llfloaterperformance.cpp
    llfloaterperms.cpp
    llfloaterprofile.cpp
    llfloaterprofilelegacy.cpp
    llfloaterpreference.cpp
    llfloaterpreferencesgraphicsadvanced.cpp
    llfloaterpreferenceviewadvanced.cpp
    llfloaterpreviewtrash.cpp
    llfloaterprofiletexture.cpp
    llfloaterprogressview.cpp
    llfloaterpublishclassified.cpp
    llfloaterregiondebugconsole.cpp
    llfloaterregioninfo.cpp
    llfloaterreportbug.cpp
    llreporttracker.cpp
    llfloaterreporter.cpp
    llfloaterregionrestarting.cpp
    llfloaterregionrestartschedule.cpp
    llfloatersavecamerapreset.cpp
    llfloatersaveoutfitas.cpp
    llfloatersaveprefpreset.cpp
    llfloatersceneloadstats.cpp
    llfloaterscriptdebug.cpp
    llfloaterscriptedprefs.cpp
    llfloaterscriptlimits.cpp
    llfloatersearch.cpp
    llfloatersearchreplace.cpp
    llfloatersellland.cpp
    llfloatersettingscolor.cpp
    llfloatersettingsdebug.cpp
    llfloatersidepanelcontainer.cpp
    llfloaterslapptest.cpp
    llfloatersnapshot.cpp
    llfloatersounddevices.cpp
    llfloaterspellchecksettings.cpp
    llfloatertelehub.cpp
    llfloatertestinspectors.cpp
    llfloatertestlistview.cpp
    llfloatertools.cpp
    llfloatertopobjects.cpp
    llfloatertos.cpp
    llfloatertoybox.cpp
    llfloatertransactionlog.cpp
    llfloatertranslationsettings.cpp
    llfloateruipreview.cpp
    llfloaterurlentry.cpp
    llfloatervoiceeffect.cpp
    llfloatervoicevolume.cpp
    llfloaterwebcontent.cpp
    llfloaterwebprofile.cpp
    llfloaterwhitelistentry.cpp
    llfloaterwindowsize.cpp
    llfloaterworldmap.cpp
    llfolderviewmodelinventory.cpp
    llfollowcam.cpp
    llfriendcard.cpp
    llflyoutcombobtn.cpp
    llflycam.cpp
    llgesturelistener.cpp
    llgesturemgr.cpp
    llgiveinventory.cpp
    llglsandbox.cpp
    llgltffolderitem.cpp
    llgltffoldermodel.cpp
    llgltfexporter.cpp
    llgltfexportwriter.cpp
    llgltfmateriallist.cpp
    llgltfmaterialpreviewmgr.cpp
    llgputexcache.cpp
    llgputexcachestore.cpp
    llgputexrecord.cpp
    llgroupactions.cpp
    llgroupcolormap.cpp # <GMDX> group-based nameplate/minimap tinting
    llgroupiconctrl.cpp
    llgrouplist.cpp
    llgroupmgr.cpp
    llhasheduniqueid.cpp
    llhints.cpp
    llhttpretrypolicy.cpp
    llhudeffect.cpp
    llhudeffectbeam.cpp
    llhudeffectlookat.cpp
    llhudeffectpointat.cpp
    llhudeffecttrail.cpp
    llhudeffectblob.cpp
    llhudeffectresetskeleton.cpp
    llhudicon.cpp
    llhudmanager.cpp
    llhudnametag.cpp
    llhudobject.cpp
    llhudrender.cpp
    llhudtext.cpp
    llhudview.cpp
    llimagefiltersmanager.cpp
    llimhandler.cpp
    llimprocessing.cpp
    llimview.cpp
    llinspect.cpp
    llinspectavatar.cpp
    llinspectgroup.cpp
    llinspectobject.cpp
    llinspectremoteobject.cpp
    llinspecttexture.cpp
    llinspecttoast.cpp
    llinventorybridge.cpp
    llinventoryfilter.cpp
    llinventoryfunctions.cpp
    llinventorygallery.cpp
    llinventorygallerymenu.cpp
    llinventoryicon.cpp
    llinventoryitemslist.cpp
    llinventorylistener.cpp
    llinventorylistitem.cpp
    llinventorymodel.cpp
    llinventorymodelbackgroundfetch.cpp
    llinventoryobserver.cpp
    llinventorypanel.cpp
    lljoystickbutton.cpp
    llkeyconflict.cpp
    lllandmarkactions.cpp
    lllandmarklist.cpp
    lllegacyatmospherics.cpp
    lllegacynotificationwellwindow.cpp
    lllistcontextmenu.cpp
    lllistview.cpp
    lllocalbitmaps.cpp
    lllocalgltfmaterials.cpp
    lllocalmesh.cpp
    lllocalanim.cpp
    lllocalassetpaths.cpp
    lllocationhistory.cpp
    lllocationinputctrl.cpp
    lllogchat.cpp
    llloginhandler.cpp
    lllogininstance.cpp
    llmachineid.cpp
    llmanip.cpp
    llmaniprotate.cpp
    llmanipscale.cpp
    llmaniptranslate.cpp
    llfloatermarketplace.cpp
    llmarketplacefunctions.cpp
    llmarketplacenotifications.cpp
    llmaterialeditor.cpp
    llmaterialmgr.cpp
    llmediactrl.cpp
    llmediadataclient.cpp
    llmenuoptionpathfindingrebakenavmesh.cpp
    llmeshbulkupload.cpp
    llmeshdiskcache.cpp
    llmeshrepository.cpp
    llmimetypes.cpp
    llmodelpreview.cpp
    llmorphview.cpp
    llmoveview.cpp
    llmutelist.cpp
    llnamebox.cpp
    llnameeditor.cpp
    llnamelistctrl.cpp
    llnavigationbar.cpp
    llfloaterimnearbychat.cpp
    llfloaterimnearbychathandler.cpp
    llfloaterimnearbychatlistener.cpp
    llnetmap.cpp
    llnotificationalerthandler.cpp
    llnotificationgrouphandler.cpp
    llnotificationhandlerutil.cpp
    llnotificationhinthandler.cpp
    llnotificationlistitem.cpp
    llnotificationlistview.cpp
    llnotificationmanager.cpp
    llnotificationofferhandler.cpp
    llnotificationscripthandler.cpp
    llnotificationstorage.cpp
    llnotificationtiphandler.cpp
    lloutfitgallery.cpp
    lloutfitslist.cpp
    lloutfitobserver.cpp
    lloutputmonitorctrl.cpp
    llpanelappearancetab.cpp
    llpanelavatar.cpp
    llpanelavatarlegacy.cpp
    llpanelavatartag.cpp
    llpanelblockedlist.cpp
    llpanelclassified.cpp
    llpanelclassifiededit.cpp
    llpanelcontents.cpp
    llpaneleditsky.cpp
    llpaneleditwater.cpp
    llpaneleditwearable.cpp
    llpanelemojicomplete.cpp
    llpanelenvironment.cpp
    llpaneleventinfo.cpp
    llpanelexperiencelisteditor.cpp
    llpanelexperiencelog.cpp
    llpanelexperiencepicker.cpp
    llpanelexperiences.cpp
    llpanelface.cpp
    llpanelgenerictip.cpp
    llpanelgroup.cpp
    llpanelgroupcreate.cpp
    llpanelgroupbulk.cpp
    llpanelgroupbulkban.cpp
    llpanelgroupexperiences.cpp
    llgroupoptions.cpp
    llpanelgroupgeneral.cpp
    llpanelgroupinvite.cpp
    llpanelgrouplandmoney.cpp
    llpanelgroupnotices.cpp
    llpanelgrouproles.cpp
    llpanelhome.cpp
    llpanelland.cpp
    llpanelobjectlinks.cpp
    llpanellandaudio.cpp
    llpanellandmarkinfo.cpp
    llpanellandmarks.cpp
    llpanellandmedia.cpp
    llpanellogin.cpp
    llpanelloginlistener.cpp
    llpanelmaininventory.cpp
    llpanelmediasettingsgeneral.cpp
    llpanelmediasettingspermissions.cpp
    llpanelmediasettingssecurity.cpp
    llpanelnearbymedia.cpp
    llpanelobject.cpp
    llpanelobjectinventory.cpp
    llpanelonlinestatus.cpp
    llpaneloutfitedit.cpp
    llpaneloutfitsinventory.cpp
    llpanelpeople.cpp
    llpanelpeoplemenus.cpp
    llpanelpermissions.cpp
    llpanelpick.cpp
    llpanelplaceinfo.cpp
    llpanelplaceprofile.cpp
    llpanelplaces.cpp
    llpanelplacestab.cpp
    llpanelpreferencegamecontrol.cpp
    alcurveeditorctrl.cpp
    ssfloatergamecontroldebug.cpp
    ssfloatergamecontroloverlay.cpp
    ssfloatergamecontrolgyro.cpp
    ssgamecontroldiagram.cpp
    ssgamecontrolui.cpp
    llpanelpresetscamerapulldown.cpp
    llpanelpresetspulldown.cpp
    llpanelprimmediacontrols.cpp
    llpanelprofile.cpp
    llpanelprofilelegacy.cpp
    llpanelprofileclassifieds.cpp
    llpanelprofilepicks.cpp
    llpanelsearchbase.cpp
    llpanelsearchclassifieds.cpp
    llpanelsearchevents.cpp
    llpanelsearchgroups.cpp
    llpanelsearchlandsales.cpp
    llpanelsearchpeople.cpp
    llpanelsearchplaces.cpp
    llpanelsearchweb.cpp
    llpanelsnapshot.cpp
    llpanelsnapshotinventory.cpp
    llpanelsnapshotlocal.cpp
    llpanelsnapshotoptions.cpp
    llpanelsnapshotpostcard.cpp
    llpanelsnapshotprofile.cpp
    llpanelteleporthistory.cpp
    fsfloaterteleporthistory.cpp
    llpaneltiptoast.cpp
    llpanelvoiceeffect.cpp
    llpaneltopinfobar.cpp
    llpanelpulldown.cpp
    llpanelvoicedevicesettings.cpp
    llpanelvolume.cpp
    llpanelvolumepulldown.cpp
    llpanelwearing.cpp
    llparcelselection.cpp
    llparticipantlist.cpp
    llpatchvertexarray.cpp
    llpathfindingcharacter.cpp
    llpathfindingcharacterlist.cpp
    llpathfindinglinkset.cpp
    llpathfindinglinksetlist.cpp
    llpathfindingmanager.cpp
    llpathfindingnavmesh.cpp
    llpathfindingnavmeshstatus.cpp
    llpathfindingnavmeshzone.cpp
    llpathfindingobject.cpp
    llpathfindingobjectlist.cpp
    llpathfindingpathtool.cpp
    llpbrterrainfeatures.cpp
    llpersistentnotificationstorage.cpp
    llphysicsmotion.cpp
    llphysicsshapebuilderutil.cpp
    llpickitem.cpp
    llpipelinelistener.cpp
    llplacesinventorybridge.cpp
    llplacesinventorypanel.cpp
    llplacesfolderview.cpp
    llpopupview.cpp
    llpostcard.cpp
    llpresetsmanager.cpp
    llpreview.cpp
    llpreviewanim.cpp
    llpreviewgesture.cpp
    llpreviewnotecard.cpp
    llpreviewscript.cpp
    llpreviewsound.cpp
    llpreviewtexture.cpp
    llproductinforequest.cpp
    llprofileimagectrl.cpp
    llprofileimagepicker.cpp
    llprogressview.cpp
    llrecentpeople.cpp
    llheroprobemanager.cpp
    llregioninfomodel.cpp
    llregionposition.cpp
    llremoteparcelrequest.cpp
    llsaveoutfitcombobtn.cpp
    llsceneview.cpp
    llscreenchannel.cpp
    llscripteditor.cpp
    llscriptfloater.cpp
    llscrollingpanelparam.cpp
    llscrollingpanelparambase.cpp
    llsculptidsize.cpp
    llsearchableui.cpp
    llsearchcombobox.cpp
    llsearchhistory.cpp
    llsecapi.cpp
    llsechandler_basic.cpp
    llselectmgr.cpp
    llsetkeybinddialog.cpp
    llsettingspicker.cpp
    llsettingsvo.cpp
    llshareavatarhandler.cpp
    llsidepanelappearance.cpp
    llsidepanelinventory.cpp
    llsidepanelinventorysubpanel.cpp
    llsidepaneliteminfo.cpp
    llsidepaneltaskinfo.cpp
    llsidetraypanelcontainer.cpp
    llskinningutil.cpp
    llsky.cpp
    llsoundsplitupload.cpp
    llslurl.cpp
    llsnapshotlivepreview.cpp
    llspatialentry.cpp
    llspeakers.cpp
    llspeakingindicatormanager.cpp
    llsplitbutton.cpp
    llstartup.cpp
    llstartuplistener.cpp
    llstatusbar.cpp
    llstylemap.cpp
    llsurface.cpp
    llsurfacepatch.cpp
    llsyntaxid.cpp
    llsyswellitem.cpp
    llsyswellwindow.cpp
    llteleporthistory.cpp
    llteleporthistorystorage.cpp
    lltexturecache.cpp
    lltexturectrl.cpp
    lltexturefetch.cpp
    lltextureinfo.cpp
    lltextureinfodetails.cpp
    lltexturestats.cpp
    lltextureview.cpp
    llthumbnailctrl.cpp
    lltinygltfhelper.cpp
    lltoast.cpp
    lltoastalertpanel.cpp
    lltoastgroupnotifypanel.cpp
    lltoastimpanel.cpp
    lltoastnotifypanel.cpp
    lltoastpanel.cpp
    lltoastscripttextbox.cpp
    lltoastscriptquestion.cpp
    lltool.cpp
    lltoolbarview.cpp
    lltoolbrush.cpp
    lltoolcomp.cpp
    lltooldraganddrop.cpp
    lltoolface.cpp
    lltoolfocus.cpp
    lltoolgrab.cpp
    lltoolgun.cpp
    lltoolindividual.cpp
    lltoolmgr.cpp
    lltoolmorph.cpp
    lltoolobjpicker.cpp
    lltoolpie.cpp
    pieautohide.cpp
    piemenu.cpp
    pieseparator.cpp
    pieslice.cpp
    lltoolpipette.cpp
    lltoolplacer.cpp
    lltoolselect.cpp
    lltoolselectland.cpp
    lltoolselectrect.cpp
    lltracker.cpp
    lltrackpicker.cpp
    lltransientdockablefloater.cpp
    lltransientfloatermgr.cpp
    lltranslate.cpp
    lluiavatar.cpp
    lluilistener.cpp
    lluploaddialog.cpp
    llurl.cpp
    llurldispatcher.cpp
    llurldispatcherlistener.cpp
    llurlfloaterdispatchhandler.cpp
    llurlhistory.cpp
    llurllineeditorctrl.cpp
    llurlwhitelist.cpp
    llversioninfo.cpp
    llviewchildren.cpp
    llviewerassetstats.cpp
    llviewerassetstorage.cpp
    llviewerassettype.cpp
    llviewerassetupload.cpp
    llviewerattachmenu.cpp
    llvieweraudio.cpp
    llviewercamera.cpp
    llviewerchat.cpp
    llviewercontrol.cpp
    llviewercontrollistener.cpp
    llviewerdisplay.cpp
    llviewerdisplayname.cpp
    llviewerfloaterreg.cpp
    llviewerfoldertype.cpp
    llviewergenericmessage.cpp
    llviewergesture.cpp
    llviewerhelp.cpp
    llviewerhelputil.cpp
    llviewerhome.cpp
    llviewerinventory.cpp
    llviewerjoint.cpp
    llviewerjointattachment.cpp
    llviewerjointmesh.cpp
    llviewerjoystick.cpp
    llviewerinput.cpp
    llviewerlayer.cpp
    llviewermedia.cpp
    llviewermedia_streamingaudio.cpp
    llviewermediafocus.cpp
    llviewermenu.cpp
    llviewermenufile.cpp
    llviewermessage.cpp
    llviewernetwork.cpp
    llviewerobject.cpp
    llviewerobjectlist.cpp
    llviewerparcelaskplay.cpp
    llviewerparcelmedia.cpp
    llviewerparcelmediaautoplay.cpp
    llviewerparcelmgr.cpp
    llviewerparceloverlay.cpp
    llviewerpartsim.cpp
    llviewerpartsource.cpp
    llviewerregion.cpp
    llviewerstats.cpp
    llviewerstatsrecorder.cpp
    llviewertexlayer.cpp
    llviewertexteditor.cpp
    llviewertexture.cpp
    llviewertextureanim.cpp
    llviewertexturelist.cpp
    llviewerthrottle.cpp
    llviewerwearable.cpp
    llviewerwindow.cpp
    llviewerwindowlistener.cpp
	llvisualeffect.cpp
    llvlcomposition.cpp
    llvlmanager.cpp
    llvkpreviewrender.cpp
    llvkautotune.cpp                  # #422 "Improve graphics speed" auto-tuner (Vulkan)
    llvkperfcapture.cpp               # #966 Performance window frame timings + captures
    llvoavatar.cpp
    llvoavatarself.cpp
    llvocache.cpp
    llvocachegrid.cpp
    llvograss.cpp
    llvoicecallhandler.cpp
    llvoicechannel.cpp
    llvoiceclient.cpp
    llvoicevisualizer.cpp
    llvoicewebrtc.cpp
    llvoinventorylistener.cpp
    llvopartgroup.cpp
    llvosky.cpp
    llvosurfacepatch.cpp
    llvotree.cpp
    llvovolume.cpp
    llvowater.cpp
    llwatchdog.cpp
    llwearableitemslist.cpp
    llwearablelist.cpp
    llweb.cpp
    llwebprofile.cpp
    llwind.cpp
    llwindowlistener.cpp
    llwlhandlers.cpp
    llworld.cpp
    llworldclock.cpp
    llworldmap.cpp
    llworldmapmessage.cpp
    llworldmipmap.cpp
    llworldmapview.cpp
    llwuffs.cpp
    llxmlrpclistener.cpp
    llxmlrpctransaction.cpp
    noise.cpp
    pipeline.cpp
    fsareasearch.cpp
    fsareasearchmenu.cpp
    rlvactions.cpp
    rlvenvironment.cpp
    rlvcommon.cpp
    rlveffects.cpp
    rlvextensions.cpp
    rlvfloaters.cpp
    rlvhandler.cpp
    rlvhelper.cpp
    rlvinventory.cpp
    rlvlocks.cpp
    rlvui.cpp
    )

# FORCE: existing build trees cached the old "alchemy-bin" name.
set(VIEWER_BINARY_NAME "slipstream-bin" CACHE STRING
    "The name of the viewer executable to create." FORCE)

set(viewer_HEADER_FILES
    lutcube.h
    CMakeLists.txt
    ViewerInstall.cmake
    slassetblocklist.h
    slfloatersoundexplorer.h
    slfloaterweather.h
    slsoundlog.h
    slweather.h
    slweatheraudio.h
    slweatherregion.h
    slparcelspeakers.h
    slparcelspeakeraudio.h
    slmediaurlpolicy.h
    slavatarcomplexitytext.h
    slavataractions.h
    slchatbar.h
    slchatcommand.h
    slfloatergenerictext.h
    slfloaterparticleeditor.h
    slfloaterregiontracker.h
    slfloaterspeakersetup.h
    slpanelmusicticker.h
    slpanelradaralert.h
    slpanelstreaminfo.h
    slstreaminfo.h
    sltpscamera.h
    slspeedfov.h
    slmmbnav.h
    sltoolalign.h
    slunzip.h
    slviewermenu.h
    bdfloatercamera.h
    fscombatads.h
    fscombataim.h
    sscombatray.h
    fscommon.h
    fscontactsfriendsmenu.h
    fsdroptarget.h
    fskeywords.h
    fscombathitmarker.h      # <GMDX> kill feed
    gmdxminimapcompass.h     # <GMDX> mini-map compass (Show Whole Region)
    gmdxminimapimage.h       # <GMDX> mini-map Copy / Save Map Image
    gmdxminimapterrain.h     # <GMDX> mini-map terrain background
    gmdxregioncapture.h      # <GMDX> region map capture
    gmdxchatlog.h      # <GMDX> chat log
    gmdxcrashreport.h      # <GMDX> crash reports
    gmdxminimapmarks.h      # <GMDX> mini-map marks
    gmdxfloaterloadouts.h    # <GMDX> loadouts window and swap toast
    gmdxloadouts.h           # <GMDX> loadouts
    gmdxreplay.h             # <GMDX> instant replay
    gmdxreplayaudio.h        # <GMDX> instant replay: the viewer's own sound, Opus
    gmdxweaponsorter.h       # <GMDX> offers to sort new weapons into the loadout folders
    gmdxrezfx.h              # <GMDX> rez effects
    gmdxselectionoutline.h   # <GMDX> LL selection silhouettes
    fsfloaterhitmarker.h     # <GMDX> hit marker
    fsfloaterkillfeed.h      # <GMDX> kill feed
    fsfloateraddtocontactset.h
    fsfavoritegroups.h
    fsfloatercontacts.h
    fspanelcontactsets.h
    fsfloatercontactsetconfiguration.h
    fsfloatergrouptitles.h
    fsfloatergroupviewer.h # <GMDX>
    fsgrouptitleregionmgr.h
    fsfloaterradar.h
    fspanelface.h
    fspanelradar.h
    fsradar.h
    fsradarentry.h
    fsradarlistctrl.h
    fsradarmenu.h
    fsscrolllistctrl.h
    lggcontactsets.h
    fsfloaterposer.h
    fsjointpose.h
    fsmaniprotatejoint.h
    fsmaniptranslatejoint.h
    fsposeranimator.h
    fsposestate.h
    fsposingmotion.h
    slaoengine.h
    slaoobjectimport.h
    slaoset.h
    slaosets.h
    slbridge.h
    slscriptinfo.h
    sldatausage.h
    slcachemetrics.h
    slassetcachestore.h
    slassetmigrate.h
    slcachemigrate.h
    slcachestore.h
    slgputexmigrate.h
    slj2cmigrate.h
    sltexturetiers.h
    slcolorwheelctrl.h
    slcolorwheelmodel.h
    slcurveeditorctrl.h
    slcurvemodel.h
    sldaycyclelandmarks.h
    sldiscordpresence.h
    slfloaterao.h
    slfloaterfirstrun.h
    slfloatergesturesets.h
    slfloaterphotostudio.h
    slfloatershapeeditor.h
    slfloaterposecreator.h
    slfloaterposestudio.h
    slfloaterviewerimport.h
    slfxini.h
    slfxpresets.h
    slkeycommands.h
    slgesturesets.h
    slgradehistory.h
    slpanelgesturesetspulldown.h
    slpanelphotostudioeffects.h
    slpanelphotostudiosection.h
    slpanelshapeeditor.h
    slphotostudiodirectory.h
    slphotostudiodraw.h
    slphotostudioscopes.h
    slphotostudiosettings.h
    slposecreator.h
    slposedialctrl.h
    slposefigurectrl.h
    slposeik.h
    slposelookat.h
    slposemath.h
    slposestudio.h
    slposesync.h
    slprimdefaults.h
    slrevocation.h
    slrevocationupdater.h
    slriskysettings.h
    slsettingsmigration.h
    slscopedata.h
    slswim.h
    sltoolscenepicker.h
    slupdateapply.h
    slupdatechecker.h
    slupdater.h
    slupdateverify.h
    slviewerimportcore.h
    slviewerimportplan.h
    slviewerpresets.h
    slwhitebalancesolver.h
    gltfscenemanager.h
    fslslpreproc.h
    fslslpreprocviewer.h
    groupchatlistener.h
    gltf/asset.h
    gltf/accessor.h
    gltf/buffer_util.h
    gltf/primitive.h
    gltf/animation.h
    gltf/llgltfloader.h
    llaccountingcost.h
    llaccountingcostmanager.h
    llaisapi.h
    llagent.h
    llagentaccess.h
    llagentbenefits.h
    llagentcamera.h
    llagentdata.h
    llagentlanguage.h
    llagentlistener.h
    llagentpicksinfo.h
    llagentpilot.h
    llagentui.h
    llagentwearables.h
    llanimstatelabels.h
    llappcorehttp.h
    llappearance.h
    llappearancelistener.h
    llappearancemgr.h
    llappviewer.h
    llappviewerlistener.h
    llattachmentsmgr.h
    llaudiosourcevo.h
    llautoreplace.h
    llavataractions.h
    llavatariconloader.h
    llavatariconctrl.h
    llavatarlist.h
    llavatarlistitem.h
    llavatarpropertiesprocessor.h
    llavatarrenderinfoaccountant.h
    llavatarrendernotifier.h
    llblockedlistitem.h
    llblocklist.h
    llbox.h
    llbuycurrencyhtml.h
    llcallingcard.h
    llcapabilityprovider.h
    llchannelmanager.h
    llchatbar.h
    llchathistory.h
    llchatimagecache.h
    llchatimagegif.h
    llchatimagewebp.h
    llchatimagepreview.h
    llchatitemscontainerctrl.h
    llchatmsgbox.h
    llchiclet.h
    llchicletbar.h
    llchunkedpurge.h
    llclassifiedinfo.h
    llclassifieditem.h
    llcofwearables.h
    llcolorswatch.h
    llcommanddispatcherlistener.h
    llcommandhandler.h
    llcommandlineparser.h
    llcommunicationchannel.h
    llcompilequeue.h
    llconfirmationmanager.h
    llcontrolavatar.h
    llconversationlog.h
    llconversationloglist.h
    llconversationloglistitem.h
    llconversationmodel.h
    llconversationview.h
    llcullcamera.h
    llcurrencyuimanager.h
    llcylinder.h
    lldateutil.h
    lldebugmessagebox.h
    lldebugview.h
    lldeferredsounds.h
    lldelayedgestureerror.h
    llderenderlist.h
    lldirpicker.h
    lldonotdisturbnotificationstorage.h
    lldndbutton.h
    lldrawable.h
    lldrawpool.h
    llcleanverify.h
    llcleaninstancestore.h
    llcleanacoustics.h
    llcleancursorpick.h
    llcleanrigdecimate.h
    llcleanclusterformat.h
    llcleanclusterbuild.h
    llcleanclustercache.h
    llcleanclustercachestore.h
    llcleanrigshadowlod.h
    llcleanpartpick.h
    llcleanassetlife.h
    llcleanmaterialstore.h
    llcleanmeshasset.h
    llcleanparticlestore.h
    llcleananimstore.h
    llcleanskinstore.h
    llcleansysavatar.h
    llcleaningest.h
    llcrossingkeep.h
    llcrossingtrace.h
    llmemdiag.h
    llmemtrendprobes.h
    llcleangltfingest.h
    llcleanterrainsmooth.h
    llcleantexture.h
    lldroptarget.h
    lldynamictexture.h
    llemote.h
    llenvironment.h
    llestateinfomodel.h
    lleventnotifier.h
    lleventpoll.h
    llexpandabletextbox.h
    llexperiencelog.h
    llexternaleditor.h
    llface.h
    llfasttimerview.h
    llfavoritesbar.h
    llfeaturemanager.h
    llfetchedgltfmaterial.h
    llfilepicker.h
    llfilteredwearablelist.h
    llfirstuse.h
    llflexibleobject.h
    llfloater360capture.h
    llfloaterabout.h
    llfloateravatarwelcomepack.h
    llfloaterbvhpreview.h
    llfloateraddpaymentmethod.h
    llfloaterauction.h
    llfloaterautoreplacesettings.h
    llfloateravatarpicker.h
    llfloateravatarrendersettings.h
    llfloateravatartextures.h
    llfloaterbanduration.h
    llfloaterbeacons.h
    llfloaterbigpreview.h
    llfloaterblocked.h
    llfloaterbuildoptions.h
    llfloaterbulkpermission.h
    llfloaterbulkupload.h
    llfloaterbump.h
    llfloaterbuy.h
    llfloaterbuycontents.h
    llfloaterbuycurrency.h
    llfloaterbuycurrencyhtml.h
    llfloaterbuyland.h
    llfloatercamerapresets.h
    llfloaterchangeitemthumbnail.h
    llfloaterchatmentionpicker.h
    llfloatercamera.h
    llfloaterchatimage.h
    llfloaterchatvoicevolume.h
    llfloaterclassified.h
    llfloatercolorpicker.h
    llfloaterconversationlog.h
    llfloaterconversationpreview.h
    llfloatercreatelandmark.h
    llfloaterdeleteprefpreset.h
    llfloaterdestinations.h
    llfloaterdirectory.h
    llfloaterdisplayname.h
    llfloatereditenvironmentbase.h
    llfloatereditextdaycycle.h
    llfloateremojipicker.h
    llfloaterenvironmentadjust.h
    llfloaterevent.h
    llfloaterexperiencepicker.h
    llfloaterexperienceprofile.h
    llfloaterexperiences.h
    llfloaterfixedenvironment.h
    llfloaterfonttest.h
    llfloaterforgetuser.h
    llfloatergesture.h
    llfloatergltfasseteditor.h
    llfloatergltfexport.h
    llfloatergodtools.h
    llfloatergotoline.h
    llfloatergridstatus.h
    llfloatergroupbulkban.h
    llfloatergroupinvite.h
    llfloatergroupprofile.h
    llfloatergroups.h
    llfloaterhandler.h
    llfloaterhelpbrowser.h
    llfloaterhoverheight.h
    llfloaterhowto.h
    llfloaterhud.h
    llfloaterimagepreview.h
    llfloaterimnearbychat.h
    llfloaterimnearbychathandler.h
    llfloaterimnearbychatlistener.h
    llfloaterimsessiontab.h
    llfloaterimsession.h
    llfloaterimcontainer.h
    llfloaterinspect.h
    llfloaterinventorysettings.h
    llfloaterinventorythumbnailshelper.h
    llfloaterjoystick.h
    llfloaterlagmeter.h
    llfloaterland.h
    llfloaterlandholdings.h
    llfloaterlinkreplace.h
    llfloaterloadprefpreset.h
    llfloaterlocalassets.h
    llfloatermap.h
    llfloatermarketplace.h
    llfloatermarketplacelistings.h
    llfloatermediasettings.h
    llfloatermemleak.h
    llfloatermeshbulkupload.h
    llfloatermodelpreview.h
    llfloatermodeluploadbase.h
    llfloatermyscripts.h
    llfloatermyenvironment.h
    llfloaternamedesc.h
    llfloaternewfeaturenotification.h
    llfloaternotificationsconsole.h
    llfloaternotificationstabbed.h
    llfloaterobjectweights.h
    llfloatertoolstexturedock.h
    llfloateropenobject.h
    llfloatersimplesnapshot.h
    llfloaterpathfindingcharacters.h
    llfloaterpathfindingconsole.h
    llfloaterpathfindinglinksets.h
    llfloaterpathfindingobjects.h
    llfloaterpay.h
    llfloaterperformance.h
    llfloaterperms.h
    llfloaterprofile.h
    llfloaterprofilelegacy.h
    llfloaterpreference.h
    llfloaterpreferencesgraphicsadvanced.h
    llfloaterpreferenceviewadvanced.h
    llfloaterpreviewtrash.h
    llfloaterprofiletexture.h
    llfloaterprogressview.h
    llfloaterpublishclassified.h
    llfloaterregiondebugconsole.h
    llfloaterregioninfo.h
    llfloaterreportbug.h
    llreporttracker.h
    llfloaterreporter.h
    llfloaterregionrestarting.h
    llfloaterregionrestartschedule.h
    llfloatersavecamerapreset.h
    llfloatersaveoutfitas.h
    llfloatersaveprefpreset.h
    llfloatersceneloadstats.h
    llfloaterscriptdebug.h
    llfloaterscriptedprefs.h
    llfloaterscriptlimits.h
    llfloatersearch.h
    llfloatersearchreplace.h
    llfloatersellland.h
    llfloatersettingscolor.h
    llfloatersettingsdebug.h
    llfloatersidepanelcontainer.h
    llfloaterslapptest.h
    llfloatersnapshot.h
    llfloatersounddevices.h
    llfloaterspellchecksettings.h
    llfloatertelehub.h
    llfloatertestinspectors.h
    llfloatertestlistview.h
    llfloatertools.h
    llfloatertopobjects.h
    llfloatertos.h
    llfloatertoybox.h
    llfloatertransactionlog.h
    llfloatertranslationsettings.h
    llfloateruipreview.h
    llfloaterurlentry.h
    llfloatervoiceeffect.h
    llfloatervoicevolume.h
    llfloaterwebcontent.h
    llfloaterwebprofile.h
    llfloaterwhitelistentry.h
    llfloaterwindowsize.h
    llfloaterworldmap.h
    llfolderviewmodelinventory.h
    llfollowcam.h
    llfriendcard.h
    llflyoutcombobtn.h
    llflycam.h
    llgesturelistener.h
    llgesturemgr.h
    llgiveinventory.h
    llgltffolderitem.h
    llgltffoldermodel.h
    llgltfexporter.h
    llgltfexportwriter.h
    llgltfmateriallist.h
    llgltfmaterialpreviewmgr.h
    llgputexcache.h
    llgputexcachestore.h
    llgputexrecord.h
    llj2crecord.h
    llgroupactions.h
    llgroupcolormap.h # <GMDX> group-based nameplate/minimap tinting
    llgroupiconctrl.h
    llgrouplist.h
    llgroupmgr.h
    llhasheduniqueid.h
    llhints.h
    llhttpretrypolicy.h
    llhudeffect.h
    llhudeffectbeam.h
    llhudeffectlookat.h
    llhudeffectpointat.h
    llhudeffecttrail.h
    llhudeffectblob.h
    llhudeffectresetskeleton.h
    llhudicon.h
    llhudmanager.h
    llhudnametag.h
    llhudobject.h
    llhudrender.h
    llhudtext.h
    llhudview.h
    llimagefiltersmanager.h
    llimprocessing.h
    llimview.h
    llinspect.h
    llinspectavatar.h
    llinspectgroup.h
    llinspectobject.h
    llinspectremoteobject.h
    llinspecttexture.h
    llinspecttoast.h
    llinventorybridge.h
    llinventoryfilter.h
    llinventoryfunctions.h
    llinventorygallery.h
    llinventorygallerymenu.h
    llinventoryicon.h
    llinventoryitemslist.h
    llinventorylistener.h
    llinventorylistitem.h
    llinventorymodel.h
    llinventorymodelbackgroundfetch.h
    llinventoryobserver.h
    llinventorypanel.h
    lljoystickbutton.h
    llkeyconflict.h
    lllandmarkactions.h
    lllandmarklist.h
    lllegacynotificationwellwindow.h
    lllightconstants.h
    lllistcontextmenu.h
    lllistview.h
    lllocalbitmaps.h
    lllocalgltfmaterials.h
    lllocalmesh.h
    lllocalanim.h
    lllocalassetpaths.h
    lllocationhistory.h
    lllocationinputctrl.h
    lllogchat.h
    llloginhandler.h
    lllogininstance.h
    llmachineid.h
    llmanip.h
    llmaniprotate.h
    llmanipscale.h
    llmaniptranslate.h
    llmarketplacefunctions.h
    llmarketplacenotifications.h
    llmaterialeditor.h
    llmaterialmgr.h
    llmediactrl.h
    llmediadataclient.h
    llmenuoptionpathfindingrebakenavmesh.h
    llmeshbulkupload.h
    llmeshdiskcache.h
    llmeshrepository.h
    llmimetypes.h
    llmodelpreview.h
    llmorphview.h
    llmoveview.h
    llmutelist.h
    llnamebox.h
    llnameeditor.h
    llnamelistctrl.h
    llnavigationbar.h
    llnetmap.h
    llnotificationhandler.h
    llnotificationlistitem.h
    llnotificationlistview.h
    llnotificationmanager.h
    llnotificationstorage.h
    lloutfitgallery.h
    lloutfitslist.h
    lloutfitobserver.h
    lloutputmonitorctrl.h
    llpanelappearancetab.h
    llpanelavatar.h
    llpanelavatarlegacy.h
    llpanelavatartag.h
    llpanelblockedlist.h
    llpanelclassified.h
    llpanelclassifiededit.h
    llpanelcontents.h
    llpaneleditsky.h
    llpaneleditwater.h
    llpaneleditwearable.h
    llpanelemojicomplete.h
    llpanelenvironment.h
    llpaneleventinfo.h
    llpanelexperiencelisteditor.h
    llpanelexperiencelog.h
    llpanelexperiencepicker.h
    llpanelexperiences.h
    llpanelface.h
    llpanelgenerictip.h
    llpanelgroup.h
    llpanelgroupcreate.h
    llpanelgroupbulk.h
    llpanelgroupbulkimpl.h
    llpanelgroupbulkban.h
    llpanelgroupexperiences.h
    llgroupoptions.h
    llpanelgroupgeneral.h
    llpanelgroupinvite.h
    llpanelgrouplandmoney.h
    llpanelgroupnotices.h
    llpanelgrouproles.h
    llpanelhome.h
    llpanelland.h
    llpanelobjectlinks.h
    llpanellandaudio.h
    llpanellandmarkinfo.h
    llpanellandmarks.h
    llpanellandmedia.h
    llpanellogin.h
    llpanelloginlistener.h
    llpanelmaininventory.h
    llpanelmediasettingsgeneral.h
    llpanelmediasettingspermissions.h
    llpanelmediasettingssecurity.h
    llpanelnearbymedia.h
    llpanelobject.h
    llpanelobjectinventory.h
    llpanelonlinestatus.h
    llpaneloutfitedit.h
    llpaneloutfitsinventory.h
    llpanelpeople.h
    llpanelpeoplemenus.h
    llpanelpermissions.h
    llpanelpick.h
    llpanelplaceinfo.h
    llpanelplaceprofile.h
    llpanelplaces.h
    llpanelplacestab.h
    llpanelpreferencegamecontrol.h
    alcurveeditorctrl.h
    ssfloatergamecontroldebug.h
    ssfloatergamecontroloverlay.h
    ssfloatergamecontrolgyro.h
    ssgamecontroldiagram.h
    ssgamecontrolui.h
    llpanelpresetscamerapulldown.h
    llpanelpresetspulldown.h
    llpanelprimmediacontrols.h
    llpanelprofile.h
    llpanelprofilelegacy.h
    llpanelprofileclassifieds.h
    llpanelprofilepicks.h
    llpanelsearchbase.h
    llpanelsearchclassifieds.h
    llpanelsearchevents.h
    llpanelsearchgroups.h
    llpanelsearchlandsales.h
    llpanelsearchpeople.h
    llpanelsearchplaces.h
    llpanelsearchweb.h
    llpanelsnapshot.h
    llpanelteleporthistory.h
    fsfloaterteleporthistory.h
    llpaneltiptoast.h
    llpanelpulldown.h
    llpanelvoicedevicesettings.h
    llpanelvoiceeffect.h
    llpaneltopinfobar.h
    llpanelvolume.h
    llpanelvolumepulldown.h
    llpanelwearing.h
    llparcelselection.h
    llparticipantlist.h
    llpatchvertexarray.h
    llpathfindingcharacter.h
    llpathfindingcharacterlist.h
    llpathfindinglinkset.h
    llpathfindinglinksetlist.h
    llpathfindingmanager.h
    llpathfindingnavmesh.h
    llpathfindingnavmeshstatus.h
    llpathfindingnavmeshzone.h
    llpathfindingobject.h
    llpathfindingobjectlist.h
    llpathfindingpathtool.h
    llpbrterrainfeatures.h
    llpersistentnotificationstorage.h
    llphysicsmotion.h
    llphysicsshapebuilderutil.h
    llpickitem.h
    llpipelinelistener.h
    llplacesinventorybridge.h
    llplacesinventorypanel.h
    llplacesfolderview.h
    llpopupview.h
    llpostcard.h
    llpresetsmanager.h
    llpreview.h
    llpreviewanim.h
    llpreviewgesture.h
    llpreviewnotecard.h
    llpreviewscript.h
    llpreviewsound.h
    llpreviewtexture.h
    llproductinforequest.h
    llprofileimagectrl.h
    llprofileimagepicker.h
    llprogressview.h
    llrecentpeople.h
    llheroprobemanager.h
    llregioninfomodel.h
    llregionposition.h
    llremoteparcelrequest.h
    llresourcedata.h
    llrootview.h
    llsaveoutfitcombobtn.h
    llsceneview.h
    llscreenchannel.h
    llscripteditor.h
    llscriptfloater.h
    llscriptruntimeperms.h
    llscrollingpanelparam.h
    llscrollingpanelparambase.h
    llsculptidsize.h
    llsearchableui.h
    llsearchcombobox.h
    llsearchhistory.h
    llsecapi.h
    llsechandler_basic.h
    llselectmgr.h
    llsetkeybinddialog.h
    llsettingspicker.h
    llsettingsvo.h
    llsidepanelappearance.h
    llsidepanelinventory.h
    llsidepanelinventorysubpanel.h
    llsidepaneliteminfo.h
    llsidepaneltaskinfo.h
    llsidetraypanelcontainer.h
    llskinningutil.h
    llsky.h
    llsoundsplitupload.h
    llslurl.h
    llsnapshotlivepreview.h
    llsnapshotmodel.h
    llspatialentry.h
    llspeakers.h
    llspeakingindicatormanager.h
    llsplitbutton.h
    llstartup.h
    llstartuplistener.h
    llstatusbar.h
    llstylemap.h
    llsurface.h
    llsurfacepatch.h
    llsyntaxid.h
    llsyswellitem.h
    llsyswellwindow.h
    lltable.h
    llteleporthistory.h
    llteleporthistorystorage.h
    llterrainpainttype.h
    llterrainsmooth.h
    lltexturecache.h
    lltexturectrl.h
    lltexturefetch.h
    lltextureinfo.h
    lltextureinfodetails.h
    lltexturestats.h
    lltextureview.h
    llthumbnailctrl.h
    lltinygltfhelper.h
    lltoast.h
    lltoastalertpanel.h
    lltoastgroupnotifypanel.h
    lltoastimpanel.h
    lltoastnotifypanel.h
    lltoastpanel.h
    lltoastscripttextbox.h
    lltoastscriptquestion.h
    lltool.h
    lltoolbarview.h
    lltoolbrush.h
    lltoolcomp.h
    lltooldraganddrop.h
    lltoolface.h
    lltoolfocus.h
    lltoolgrab.h
    lltoolgun.h
    lltoolindividual.h
    lltoolmgr.h
    lltoolmorph.h
    lltoolobjpicker.h
    lltoolpie.h
    pieautohide.h
    piemenu.h
    pieseparator.h
    pieslice.h
    lltoolpipette.h
    lltoolplacer.h
    lltoolselect.h
    lltoolselectland.h
    lltoolselectrect.h
    lltracker.h
    lltrackpicker.h
    lltransientdockablefloater.h
    lltransientfloatermgr.h
    lltranslate.h
    lluiconstants.h
    lluiavatar.h
    lluilistener.h
    lluploaddialog.h
    lluploadfloaterobservers.h
    llurl.h
    llurldispatcher.h
    llurldispatcherlistener.h
    llurlfloaterdispatchhandler.h
    llurlhistory.h
    llurllineeditorctrl.h
    llurlwhitelist.h
    llversioninfo.h
    llviewchildren.h
    llviewerassetstats.h
    llviewerassetstorage.h
    llviewerassettype.h
    llviewerassetupload.h
    llviewerattachmenu.h
    llvieweraudio.h
    llviewercamera.h
    llviewerchat.h
    llviewercontrol.h
    llviewercontrollistener.h
    llviewerdisplay.h
    llviewerdisplayname.h
    llviewerfloaterreg.h
    llviewerfoldertype.h
    llviewergenericmessage.h
    llviewergesture.h
    llviewerhelp.h
    llviewerhome.h
    llviewerinventory.h
    llviewerjoint.h
    llviewerjointattachment.h
    llviewerjointmesh.h
    llviewerjoystick.h
    llviewerinput.h
    llviewerlayer.h
    llviewermedia.h
    llviewermediafocus.h
    llviewermediaobserver.h
    llviewermenu.h
    llviewermenufile.h
    llviewermessage.h
    llviewernetwork.h
    llviewerobject.h
    llviewerobjectlist.h
    llviewerparcelaskplay.h
    llviewerparcelmedia.h
    llviewerparcelmediaautoplay.h
    llviewerparcelmgr.h
    llviewerparceloverlay.h
    llviewerpartsim.h
    llviewerpartsource.h
    llviewerprecompiledheaders.h
    llviewerregion.h
    llviewerstats.h
    llviewerstatsrecorder.h
    llviewertexlayer.h
    llviewertexteditor.h
    llviewertexture.h
    llviewertextureanim.h
    llviewertexturelist.h
    llviewerthrottle.h
    llviewerwearable.h
    llviewerwindow.h
    llviewerwindowlistener.h
	llvisualeffect.h
    llvlcomposition.h
    llvlmanager.h
    llvkpreviewrender.h
    llvkautotune.h
    llvkperfcapture.h
    llvoavatar.h
    llvoavatarself.h
    llvocache.h
    llvocachegrid.h
    llvograss.h
    llvoicechannel.h
    llvoiceclient.h
    llvoicevisualizer.h
    llvoicewebrtc.h
    llvoinventorylistener.h
    llvopartgroup.h
    llvosky.h
    llvosurfacepatch.h
    llvotree.h
    llvovolume.h
    llvowater.h
    llwatchdog.h
    llwearableitemslist.h
    llwearablelist.h
    llweb.h
    llwebprofile.h
    llwind.h
    llwindowlistener.h
    llwlhandlers.h
    llworld.h
    llworldclock.h
    llworldmap.h
    llworldmapmessage.h
    llworldmipmap.h
    llwuffs.h
    llworldmapview.h
    llxmlrpclistener.h
    llxmlrpctransaction.h
    noise.h
    pipeline.h
    fsareasearch.h
    fsareasearchmenu.h
    rlvactions.h
    rlvcommon.h
    rlvdefines.h
    rlveffects.h
    rlvenvironment.h
    rlvextensions.h
    rlvfloaters.h
    rlvhandler.h
    rlvhelper.h
    rlvinventory.h
    rlvlocks.h
    rlvmodifiers.h
    rlvui.h
    roles_constants.h
    VertexCache.h
    VorbisFramework.h
    )

  list(APPEND viewer_SOURCE_FILES llperfstats.cpp)
  list(APPEND viewer_HEADER_FILES llperfstats.h)

if(SLIPSTREAM_TESTCTL)
  list(APPEND viewer_SOURCE_FILES sltestctl.cpp)
  list(APPEND viewer_HEADER_FILES sltestctl.h)
  # Only these two files see SL_TESTCTL, so toggling the switch rebuilds them, not newview.
  set_property(SOURCE sltestctl.cpp llappviewer.cpp APPEND PROPERTY COMPILE_DEFINITIONS SL_TESTCTL=1)
endif()

# EXPERIMENTAL Vulkan integration (vkgfx). Additive; GL is the default and the
# Vulkan path is behind the RenderUseVulkan setting / --vulkan arg.
list(APPEND viewer_SOURCE_FILES
    vkgfx/vk_device.cpp
    vkgfx/vk_requirements.cpp         # #526 Vulkan minimum: per-device check, ranking, "can't run" text
    vkgfx/vk_compat.cpp               # #675 Vulkan 1.2 + extensions entry-point routing / SPIR-V 1.5 downgrade
    vkgfx/vk_vma.cpp                  # Vulkan Memory Allocator implementation (texture pools, #411)
    vkgfx/vk_swapchain.cpp
    vkgfx/vk_pipeline.cpp             # graphics/compute pipeline helpers (world pass)
    vkgfx/vk_descpool.cpp             # layout-derived descriptor pools + allocation accounting
    vkgfx/vk_pipecache.cpp            # persistent (disk) VkPipelineCache: load / validate / save
    vkgfx/llvrambudgetvk.cpp          # one VRAM budget: status + soft pressure ladder (#405, #407)
    vkgfx/llcleanrenderervk.cpp       # GPU-driven static-opaque world render (VK)
    vkgfx/llcleanspecvk.cpp           # graphics settings as specialization constants: spec sets, LRU, apply
    vkgfx/llcleanrtvk.cpp             # ray tracing foundation (BLAS/TLAS, ray-query debug views)
    vkgfx/llcleanrtomm.cpp            # opacity micromaps (#194)
    vkgfx/llcleanrtaudit.cpp          # DEVICE_LOST RT scene audit (#874)
    vkgfx/llcleangodraysvk.cpp        # god rays / volumetric sun shafts (#208)
    vkgfx/llcleanvolfogvk.cpp         # volumetric fog: froxel grid + local volumes (#718)
    vkgfx/llcleanpartfogvk.cpp        # particle fog: fog-like particle systems as dense bricks (#1003)
    vkgfx/llcleanvolcloudsvk.cpp      # volumetric sky cloud layer (#719)
    vkgfx/llcleanweathervk.cpp        # weather: precipitation, occlusion map, wet surfaces (#783)
    vkgfx/llcleanweathervk_runoff.cpp # weather run-off: flow, drips, streams (#783 M4)
    vkgfx/llcleanweathervk_movers.cpp # weather mover tracks: vehicles / movers plough the snow (#783 M5)
    vkgfx/llcleanweathervk_shelter.cpp # weather per-avatar shelter maps: umbrellas, hats, avatar snow (#901)
    vkgfx/llcleanweathervk_accum.cpp  # weather per-avatar rest-pose accumulation of snow / wetness (#901)
    vkgfx/llcleanpartlowresvk.cpp     # reduced-resolution particles (RenderVKParticleLowRes)
    vkgfx/llcleanmotionblurvk.cpp     # motion blur (#118, tile reconstruction filter)
    vkgfx/llcleansnapshotvk.cpp       # live snapshots: main-render-path capture (#873)
    vkgfx/llcleanrigweldvk.cpp        # rigged vertex welding host side (#574)
    vkgfx/llcleanrtdenoisevk.cpp      # RT shadow denoiser a-trous + debug views (#85)
    vkgfx/llcleandenoisevk.cpp        # diffuse / specular lighting denoiser (#85 phase 2)
    vkgfx/llcleanprojstochvk.cpp      # RT projectors stochastic + denoised (#85 phase 2)
    vkgfx/llcleangivk.cpp             # ray-traced GI: ReSTIR GI + radiance cache (#87)
    vkgfx/llcleanrtcachevk.cpp        # RT sun shadow cache: tile classifier + indirect trace (#771)
    vkgfx/llcleansuncertvk.cpp        # sun exposure certificates: world-space sun shadow cache (#771 Phase 2)
    vkgfx/llcleanterrainbakevk.cpp    # per-region terrain bake: far terrain from baked textures (#811)
    vkgfx/gmdxrezfxvk.cpp             # <GMDX> rez effects: the effect table upload
    vkgfx/gmdxseloutlinevk.cpp        # <GMDX> selection styles: classic wireframe, screen-space outline
    vkgfx/gmdxphysskyvk.cpp           # <GMDX> physical sky (Hillaire): parameters, lookup tables
    vkgfx/gmdxreplayvk.cpp            # <GMDX> instant replay: GPU H.264 encode + MP4
    vkgfx/llcelshadevk.cpp            # avatar mask + cel shading (#549 / #550)
    vkgfx/llcleanpickvk.cpp           # GPU cursor pick of rigged surfaces (#255)
    vkgfx/llcleanaudiortvk.cpp        # acoustics probe: room + occlusion rays against the TLAS (#683)
    vkgfx/llcleantexencvk.cpp         # GPU texture block compression (#263)
    vkgfx/llcleanclustervk.cpp        # cluster LOD GPU module owner (#743)
    vkgfx/llcleanclustercaps.cpp      # cluster LOD capability tiers (#743)
    vkgfx/llcleanclusterselectvk.cpp  # cluster LOD selection / culling / emission (#745)
    vkgfx/llcleanclusterstreamvk.cpp  # cluster LOD streaming / residency (#746)
    vkgfx/llcleanclustercontrolvk.cpp # cluster LOD tolerance controller (#749)
    vkgfx/llcleanclusterskinvk.cpp    # cluster LOD skinning / avatar deformation (#747)
    vkgfx/llcleanclusterrtvk.cpp      # cluster LOD ray tracing: RT cut, CLAS / per-cut BLAS, PTLAS (#748)
    vkgfx/llcleanfsr3vk.cpp           # AMD FSR 3.1 upscaler host side (RenderVKUpscaler 1-6)
    vkgfx/llvkneuralrecon.cpp         # #870 the optional slip_nr library (user-built DLSS RR shim, run-time loaded)
    vkgfx/llcleandlssrrvk.cpp         # #870 DLSS Ray Reconstruction host side (RenderVKUpscaler 7-11)
    vkgfx/llfxcompile.cpp             # ReShade FX compile front end (#543; reshadefx -> SPIR-V)
    vkgfx/llfxruntimevk.cpp           # built-in ReShade FX effects runtime on the post chain (#544)
    slfxstack.cpp                     # effects stack: effect list, techniques, values, safety (#544)
    vkgfx/llvulkancontext.cpp
    vkgfx/llrendervkbackend.cpp
    # The proven LLRenderVK facade (immediate-mode + state + font/text path).
    # Its vk_device/vk_common are unified with vkgfx/ above; we do NOT compile
    # vkgfx/llrendervk/vk_device.cpp here (the viewer's vkgfx/vk_device.cpp is used).
    vkgfx/llrendervk/llrendervk.cpp
    vkgfx/llrendervk/vk_pipeline_ui.cpp
    vkgfx/llrendervk/vk_offscreen.cpp
    )
list(APPEND viewer_HEADER_FILES
    vkgfx/vk_common.h
    vkgfx/vk_device.h
    vkgfx/vk_requirements.h
    vkgfx/vk_vma.h
    vkgfx/vk_compat.h
    vkgfx/third_party/vma/vk_mem_alloc.h
    vkgfx/vk_swapchain.h
    vkgfx/vk_pipeline.h
    vkgfx/vk_descpool.h
    vkgfx/vk_stagering.h
    vkgfx/vk_pipecache.h
    vkgfx/llvrambudgetvk.h
    vkgfx/llcleanrenderervk.h
    vkgfx/llcleanrtvk.h
    vkgfx/llcleanrtomm.h
    vkgfx/llcleangodraysvk.h
    vkgfx/llcleandenoisevk.h
    vkgfx/llcleanvolfogvk.h
    vkgfx/llcleanpartfogvk.h
    vkgfx/llcleanvolcloudsvk.h
    vkgfx/llcleanweathervk.h
    vkgfx/llcleanpartlowresvk.h
    vkgfx/llcleanmotionblurvk.h
    vkgfx/llcleansnapshotvk.h
    vkgfx/llcelshadevk.h
    vkgfx/llcleanpickvk.h
    vkgfx/llcleanaudiortvk.h
    vkgfx/llcleantexencvk.h
    vkgfx/llcleanclustervk.h
    vkgfx/llcleanclustercaps.h
    vkgfx/llcleanclusterselectvk.h
    vkgfx/llcleanclusterstreamvk.h
    vkgfx/llcleanclustercontrolvk.h
    vkgfx/llcleanclusterskinvk.h
    vkgfx/llcleanrigrestvk.h
    vkgfx/llcleanclusterrtvk.h
    vkgfx/llcleanfsr3vk.h
    vkgfx/llvkneuralrecon.h
    vkgfx/llcleandlssrrvk.h
    vkgfx/llfxcompile.h
    vkgfx/llfxruntimevk.h
    slfxstack.h
    vkgfx/llvulkancontext.h
    vkgfx/llrendervkbackend.h
    vkgfx/llvkdyntex.h
    vkgfx/llrendervk/llrendervk.h
    vkgfx/llrendervk/vk_offscreen.h
    vkgfx/llrendervk/vk_pipeline_ui.h
    )
# Embedded SPIR-V (vk_world_shaders_spv.h, vk_fsr3_shaders_spv.h, vk_world_shaders_hash.h,
# vk_fx_shaders_spv.h, vk_ui_shaders_spv.h): compiled from the GLSL at build time into
# ${VK_SHADER_GEN_DIR}, never committed. Needs glslc (shaderc) and glslangValidator (glslang)
# -- Arch: shaderc glslang; Debian/Ubuntu: glslc glslang-tools. The generators run their
# variants in parallel and cache them by preprocessed source (indra/vkclean/spvbuild.py), and
# only rewrite a header whose content changed (byproducts: Ninja restat), so editing one
# shader recompiles only the C++ that embeds the changed header.
find_program(GLSLC_EXECUTABLE glslc)
find_program(GLSLANG_VALIDATOR_EXECUTABLE glslangValidator)
if (NOT GLSLC_EXECUTABLE OR NOT GLSLANG_VALIDATOR_EXECUTABLE)
  message(FATAL_ERROR "The Vulkan shader tools are required to build the viewer: glslc "
                      "(${GLSLC_EXECUTABLE}) and glslangValidator (${GLSLANG_VALIDATOR_EXECUTABLE}). "
                      "Arch / CachyOS: pacman -S shaderc glslang; Debian / Ubuntu: apt install glslc glslang-tools.")
endif ()
set(VK_SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/vkshaders")
file(MAKE_DIRECTORY "${VK_SHADER_GEN_DIR}")
set(_vk_shader_env "${CMAKE_COMMAND}" -E env "GLSLC=${GLSLC_EXECUTABLE}"
    "GLSLANG_VALIDATOR=${GLSLANG_VALIDATOR_EXECUTABLE}")
set(_vk_spv_common
    "${CMAKE_SOURCE_DIR}/vkclean/spvbuild.py"
    "${CMAKE_SOURCE_DIR}/vkclean/push_budget.py")
file(GLOB_RECURSE _vk_world_glsl CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/vkclean/shaders/*"
    "${CMAKE_SOURCE_DIR}/vkclean/third_party/fidelityfx/gpu/*"
    "${CMAKE_SOURCE_DIR}/vkclean/third_party/fidelityfx/shaders/*")
file(GLOB _vk_fx_glsl CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/fxshaders/*")
file(GLOB _vk_ui_glsl CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/llrendervk/shaders/*")
add_custom_command(
    OUTPUT "${VK_SHADER_GEN_DIR}/world.stamp"
    BYPRODUCTS "${VK_SHADER_GEN_DIR}/vk_world_shaders_spv.h" "${VK_SHADER_GEN_DIR}/vk_fsr3_shaders_spv.h"
               "${VK_SHADER_GEN_DIR}/vk_world_shaders_hash.h" "${VK_SHADER_GEN_DIR}/vk_world_shaders_spv.cpp"
    COMMAND ${_vk_shader_env} "${PYTHON_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/vkclean/regen_world_spv.py"
            --out-dir "${VK_SHADER_GEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${VK_SHADER_GEN_DIR}/world.stamp"
    DEPENDS "${CMAKE_SOURCE_DIR}/vkclean/regen_world_spv.py" ${_vk_spv_common} ${_vk_world_glsl}
    COMMENT "Compiling the Vulkan world + FSR shaders (glslc)"
    VERBATIM)
add_custom_command(
    OUTPUT "${VK_SHADER_GEN_DIR}/fx.stamp"
    BYPRODUCTS "${VK_SHADER_GEN_DIR}/vk_fx_shaders_spv.h"
    COMMAND ${_vk_shader_env} "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/fxshaders/regen_fx_spv.py"
            --out-dir "${VK_SHADER_GEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${VK_SHADER_GEN_DIR}/fx.stamp"
    DEPENDS ${_vk_spv_common} ${_vk_fx_glsl}
    COMMENT "Compiling the Vulkan FX host shaders (glslc)"
    VERBATIM)
add_custom_command(
    OUTPUT "${VK_SHADER_GEN_DIR}/ui.stamp"
    BYPRODUCTS "${VK_SHADER_GEN_DIR}/vk_ui_shaders_spv.h"
    COMMAND ${_vk_shader_env} "${PYTHON_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/llrendervk/regen_ui_spv.py"
            --out-dir "${VK_SHADER_GEN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${VK_SHADER_GEN_DIR}/ui.stamp"
    DEPENDS ${_vk_spv_common} ${_vk_ui_glsl}
    COMMENT "Compiling the Vulkan UI shaders (glslc)"
    VERBATIM)
add_custom_target(vk_shaders DEPENDS
    "${VK_SHADER_GEN_DIR}/world.stamp" "${VK_SHADER_GEN_DIR}/fx.stamp" "${VK_SHADER_GEN_DIR}/ui.stamp")

# The facade sources include "vk_device.h"/"vk_common.h"/"vk_offscreen.h" by
# bare name; add its dir to the include path (unified headers, no shadowing).
# NOTE: setting INCLUDE_DIRECTORIES per-source REPLACES the target's default
# include dirs for that source, so any bare-name include of a newview header
# must have ${CMAKE_CURRENT_SOURCE_DIR} added back explicitly (see the clean
# renderer below, which includes the llclean*store headers).
# The VMA implementation TU (#411): third-party code, no viewer precompiled header (it
# includes nothing of the viewer; keeps windows.h / LL macros away from VMA).
set_source_files_properties(
    vkgfx/vk_vma.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
# The ReShade FX compile front end (#543): pure C++ over the vendored reshadefx (no
# viewer headers), so no viewer precompiled header either.
set_source_files_properties(
    vkgfx/llfxcompile.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
# <GMDX> Instant replay audio: std + OS audio APIs + llwebrtc's Opus export, so no viewer
# precompiled header.
set_source_files_properties(
    gmdxreplayaudio.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
# Auto-updater (#421): the signature check and the Linux apply step use only std, OpenSSL
# and Boost.JSON (they also run as a command-line mode and in scripts/updater tests), so
# no viewer precompiled header. The update signing PUBLIC key is compiled into
# slupdateverify.cpp when configured with -DUPDATE_PUBKEY_HEX=<64 hex>; without it the
# updater refuses every manifest.
set_source_files_properties(
    slupdateverify.cpp
    slupdateapply.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
# Certificate revocation (CRLSet / distrust list / CRX3): std, OpenSSL, zlib and Boost.JSON
# only, also built standalone by scripts/revocation/test_revocation.sh.
set_source_files_properties(
    slrevocation.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
# The .gtx -> cache store migration (#714) and the GPU-ready record layout (#865):
# llcommon + llcachestore only (their unit tests
# link them without the viewer), so no viewer precompiled header.
set_source_files_properties(
    slgputexmigrate.cpp
    llgputexrecord.cpp
    slj2cmigrate.cpp
    slcachemigrate.cpp
    slassetmigrate.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS TRUE)
if (UPDATE_PUBKEY_HEX)
  if (NOT UPDATE_PUBKEY_HEX MATCHES "^[0-9a-fA-F][0-9a-fA-F]+$" OR NOT UPDATE_PUBKEY_HEX MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "UPDATE_PUBKEY_HEX must be the 64 hex characters of the Ed25519 public key")
  endif ()
  string(LENGTH "${UPDATE_PUBKEY_HEX}" _update_key_len)
  if (NOT _update_key_len EQUAL 64)
    message(FATAL_ERROR "UPDATE_PUBKEY_HEX must be 64 hex characters, got ${_update_key_len}")
  endif ()
  set_source_files_properties(
      slupdateverify.cpp
      PROPERTIES COMPILE_DEFINITIONS "SLIPSTREAM_UPDATE_PUBKEY_HEX=\"${UPDATE_PUBKEY_HEX}\"")
endif ()
# Jetstream builds (channel "Slipstream Jetstream", built every 6 hours from main by
# .forgejo/workflows/jetstream.yml) keep their own updater state and only take jetstream
# update manifests (slupdateapply.h).
if (VIEWER_CHANNEL MATCHES "Jetstream")
  set_property(SOURCE slupdateapply.cpp APPEND PROPERTY COMPILE_DEFINITIONS SLIPSTREAM_JETSTREAM=1)
endif ()
# The Wuffs implementation TU (#265, GIF chat previews): generated third-party
# code compiled as C++ (third_party/wuffs/README.md). No viewer precompiled
# header, and its warnings are not ours to fix (-Werror / /WX would trip on new
# compilers): warnings off for this one file only.
if (MSVC)
  set(LL_WUFFS_NO_WARNINGS "/w")
else ()
  set(LL_WUFFS_NO_WARNINGS "-w")
endif ()
set_source_files_properties(
    llwuffs.cpp
    PROPERTIES
        SKIP_PRECOMPILE_HEADERS TRUE
        COMPILE_FLAGS "${LL_WUFFS_NO_WARNINGS}")
set_source_files_properties(
    vkgfx/llrendervk/llrendervk.cpp
    vkgfx/llrendervk/vk_pipeline_ui.cpp
    vkgfx/llrendervk/vk_offscreen.cpp
    vkgfx/llrendervkbackend.cpp
    vkgfx/llvulkancontext.cpp
    vkgfx/vk_pipeline.cpp
    vkgfx/vk_descpool.cpp
    PROPERTIES INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/llrendervk;${CMAKE_CURRENT_SOURCE_DIR}/vkgfx;${VK_SHADER_GEN_DIR}")

# #870: the slip_nr ABI header is shared with the MIT shim project (contrib/ngxshim, NOT built
# here): only this TU includes it.
set_source_files_properties(
    vkgfx/llvkneuralrecon.cpp
    PROPERTIES INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/llrendervk;${CMAKE_CURRENT_SOURCE_DIR}/vkgfx;${CMAKE_SOURCE_DIR}/../contrib/ngxshim/include")

# The clean-pipeline C++ stores (llclean*store) are part of this branch, so the
# VK world renderer's syncFromStores() can read their live CPU arrays. Flip on
# the LL_HAS_CLEAN_PIPELINE seam for that ONE TU (it includes the store headers,
# which live in ${CMAKE_CURRENT_SOURCE_DIR} = newview). Give this source the
# newview dir AND the vkgfx dirs (the per-source list replaces the target's, so
# newview must be listed explicitly or the store headers won't resolve). On a
# branch lacking the clean pipeline this define is absent and the renderer uses
# its representative prototype scene instead.
set_source_files_properties(
    vkgfx/llcleanrenderervk.cpp
    vkgfx/llcleanspecvk.cpp
    vkgfx/llcleanrtvk.cpp
    vkgfx/llcleanrtomm.cpp
    vkgfx/llcleanrtaudit.cpp
    vkgfx/llcleangodraysvk.cpp
    vkgfx/llcleanvolfogvk.cpp
    vkgfx/llcleanpartfogvk.cpp
    vkgfx/llcleanvolcloudsvk.cpp
    vkgfx/llcleanweathervk.cpp
    vkgfx/llcleanweathervk_runoff.cpp
    vkgfx/llcleanweathervk_movers.cpp
    vkgfx/llcleanweathervk_shelter.cpp
    vkgfx/llcleanweathervk_accum.cpp
    vkgfx/llcleanmotionblurvk.cpp
    vkgfx/llcleansnapshotvk.cpp
    vkgfx/llcleanrigweldvk.cpp
    vkgfx/llcleanrtdenoisevk.cpp
    vkgfx/llcleandenoisevk.cpp
    vkgfx/llcleanprojstochvk.cpp
    vkgfx/llcleangivk.cpp
    vkgfx/llcleanrtcachevk.cpp
    vkgfx/llcleansuncertvk.cpp
    vkgfx/llcleanterrainbakevk.cpp
    vkgfx/gmdxrezfxvk.cpp   # <GMDX>
    vkgfx/gmdxseloutlinevk.cpp   # <GMDX>
    vkgfx/gmdxphysskyvk.cpp   # <GMDX>
    vkgfx/llcelshadevk.cpp
    vkgfx/llcleanpickvk.cpp
    vkgfx/llcleanaudiortvk.cpp
    vkgfx/llcleantexencvk.cpp
    vkgfx/llcleanclustervk.cpp
    vkgfx/llcleanclustercaps.cpp
    vkgfx/llcleanclusterselectvk.cpp
    vkgfx/llcleanclusterstreamvk.cpp
    vkgfx/llcleanclustercontrolvk.cpp
    vkgfx/llcleanclusterskinvk.cpp
    vkgfx/llcleanclusterrtvk.cpp
    PROPERTIES
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/llrendervk;${CMAKE_CURRENT_SOURCE_DIR}/vkgfx;${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/third_party/reshadefx/src;${VK_SHADER_GEN_DIR}"
        COMPILE_DEFINITIONS "LL_HAS_CLEAN_PIPELINE=1")
# Built-in effects stack (#544): runtime + stack (bare vkgfx includes + reshadefx headers).
set_source_files_properties(
    vkgfx/llfxruntimevk.cpp
    slfxstack.cpp
    PROPERTIES
        INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/llrendervk;${CMAKE_CURRENT_SOURCE_DIR}/vkgfx;${CMAKE_CURRENT_SOURCE_DIR}/vkgfx/third_party/reshadefx/src;${VK_SHADER_GEN_DIR}")

source_group("CMake Rules" FILES ViewerInstall.cmake)

#build_data.json creation moved to viewer_manifest.py MAINT-6413
# the viewer_version.txt file created here is for passing to viewer_manifest and autobuild
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt"
           "${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION}\n")

set_source_files_properties(
   llversioninfo.cpp tests/llversioninfo_test.cpp
   PROPERTIES
   COMPILE_DEFINITIONS "${VIEWER_CHANNEL_VERSION_DEFINES}" # see BuildVersion.cmake
   )

if (DARWIN)
  LIST(APPEND viewer_SOURCE_FILES llappviewermacosx.cpp)
  LIST(APPEND viewer_SOURCE_FILES llappviewermacosx-objc.mm)
  LIST(APPEND viewer_SOURCE_FILES llappviewermacosx-objc.h)
  LIST(APPEND viewer_SOURCE_FILES llfilepicker_mac.mm)
  LIST(APPEND viewer_HEADER_FILES llfilepicker_mac.h)

  set_source_files_properties(
    llappviewermacosx-objc.mm
    PROPERTIES
    SKIP_PRECOMPILE_HEADERS TRUE
  )

  set_source_files_properties(
    llfilepicker_mac.mm
    PROPERTIES
    SKIP_PRECOMPILE_HEADERS TRUE
  )

  # This should be compiled with the viewer.
  LIST(APPEND viewer_SOURCE_FILES llappdelegate-objc.mm)
  set_source_files_properties(
    llappdelegate-objc.mm
    PROPERTIES
    SKIP_PRECOMPILE_HEADERS TRUE
    COMPILE_DEFINITIONS "${VIEWER_CHANNEL_VERSION_DEFINES}"
    # BugsplatMac is a module, imported with @import. That language feature
    # demands these -f switches.
    # Xcode 10.2 requires that Objective-C++ headers declare nullability of
    # pointer variables. As of 2019-06-26, the BugsplatMac version we're using
    # does not yet do so in its own header files. This -W flag prevents fatal
    # warnings.
    COMPILE_FLAGS "-fmodules -fcxx-modules -Wno-nullability-completeness"
    )

  # Add resource files to the project.
  set(viewer_RESOURCE_FILES
    Info-SecondLife.plist
    SecondLife.xib
    # CMake doesn't seem to support Xcode language variants well just yet
    English.lproj/InfoPlist.strings
    English.lproj/language.txt
    German.lproj/language.txt
    Japanese.lproj/language.txt
    Korean.lproj/language.txt
    )

  SOURCE_GROUP("Resources" FILES ${viewer_RESOURCE_FILES})
  list(APPEND viewer_SOURCE_FILES ${viewer_RESOURCE_FILES})
endif (DARWIN)

if (LINUX)
    LIST(APPEND viewer_SOURCE_FILES llappviewerlinux.cpp)
    set_source_files_properties(
      llappviewerlinux.cpp
      PROPERTIES
      COMPILE_DEFINITIONS "${VIEWER_CHANNEL_VERSION_DEFINES}"
      )
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed")

endif (LINUX)

if (WINDOWS)

    list(APPEND viewer_SOURCE_FILES
         llappviewerwin32.cpp
         llwindebug.cpp
         slwin7compat.cpp
         )
    set_source_files_properties(
      llappviewerwin32.cpp
      PROPERTIES
      COMPILE_DEFINITIONS "${VIEWER_CHANNEL_VERSION_DEFINES}"
      )

    list(APPEND viewer_HEADER_FILES
         llappviewerwin32.h
         llwindebug.h
         )

    # Replace the icons with the appropriate ones for the channel
    # ('test' is the default)
    set(ICON_PATH "test")
    set(VIEWER_MACOSX_PHASE "d")
    string(TOLOWER ${VIEWER_CHANNEL} channel_lower)
    if(channel_lower MATCHES "^slipstream release")
        set(ICON_PATH "release")
        set(VIEWER_MACOSX_PHASE "f")
    elseif(channel_lower MATCHES "^slipstream beta")
        set(ICON_PATH "beta")
        set(VIEWER_MACOSX_PHASE "b")
    elseif(channel_lower MATCHES "^slipstream project")
        set(ICON_PATH "project")
        set(VIEWER_MACOSX_PHASE "a")
    endif()
    # Slipstream branding: one in-tree icon set for every channel (indra/newview/icons/slipstream).
    set(SLIPSTREAM_ICON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/icons/slipstream")
    message(STATUS "Copying icons for ${ICON_PATH}")
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLIPSTREAM_ICON_DIR}/slipstream.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/res/ll_icon.ico"
      )
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLIPSTREAM_ICON_DIR}/slipstream_256.BMP"
        "${CMAKE_CURRENT_SOURCE_DIR}/res/ll_icon.BMP"
      )
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLIPSTREAM_ICON_DIR}/slipstream_256.BMP"
        "${CMAKE_CURRENT_SOURCE_DIR}/res-sdl/ll_icon.BMP"
      )
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLIPSTREAM_ICON_DIR}/slipstream.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/installers/windows/install_icon.ico"
      )
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLIPSTREAM_ICON_DIR}/slipstream.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/installers/windows/uninstall_icon.ico"
      )
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SLIPSTREAM_ICON_DIR}/slipstream_256.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/installers/windows/install_wizard_small.png"
      )

    # Add resource files to the project.
    # viewerRes.rc is the only buildable file, but
    # the rest are all dependencies of it.
    set(viewer_RESOURCE_FILES
        res/arrow.cur
        res/arrowcop.cur
        res/arrowcopmulti.cur
        res/arrowdrag.cur
        res/circleandline.cur
        res/icon1.ico
        res/llarrow.cur
        res/llarrowdrag.cur
        res/llarrowdragmulti.cur
        res/llarrowlocked.cur
        res/llgrablocked.cur
        res/llno.cur
        res/llnolocked.cur
        res/lltoolcamera.cur
        res/lltoolcreate.cur
        res/lltoolfocus.cur
        res/lltoolgrab.cur
        res/lltoolland.cur
        res/lltoolpan.cur
        res/lltoolpathfinding.cur
        res/lltoolpathfindingpathend.cur
        res/lltoolpathfindingpathendadd.cur
        res/lltoolpathfindingpathstart.cur
        res/lltoolpathfindingpathstartadd.cur
        res/lltoolpipette.cur
        res/lltoolrotate.cur
        res/lltoolscale.cur
        res/lltooltranslate.cur
        res/lltoolzoomin.cur
        res/lltoolzoomout.cur
        res-sdl/ll_icon.BMP
        res/ll_icon.BMP
        res/ll_icon.ico
        res/resource.h
        res/toolpickobject.cur
        res/toolpickobject2.cur
        res/toolpickobject3.cur
        res/toolpipette.cur
        res/toolbuy.cur
        res/toolopen.cur
        res/toolsit.cur
        )

    set_source_files_properties(${viewer_RESOURCE_FILES}
                                PROPERTIES HEADER_FILE_ONLY TRUE)

    configure_file( ${CMAKE_CURRENT_SOURCE_DIR}/res/viewerRes.rc
                    ${CMAKE_CURRENT_BINARY_DIR}/viewerRes.rc
                    )
    set(viewer_RESOURCE_FILES
        ${CMAKE_CURRENT_BINARY_DIR}/viewerRes.rc
        ${viewer_RESOURCE_FILES}
        )

    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/viewerRes.rc
      PROPERTIES COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/res"
      )

    SOURCE_GROUP("Resource Files" FILES ${viewer_RESOURCE_FILES})

    list(APPEND viewer_SOURCE_FILES ${viewer_RESOURCE_FILES})

    find_library(INTEL_MEMOPS_LIBRARY
                 NAMES ll_intel_memops
                 PATHS
                 optimized ${ARCH_PREBUILT_DIRS_RELEASE}
                 debug ${ARCH_PREBUILT_DIRS_DEBUG}
                 )
    mark_as_advanced(INTEL_MEMOPS_LIBRARY)


    if (INTEL_MEMOPS_LIBRARY)
      add_library( ll::intel_memops INTERFACE IMPORTED )
      target_link_libraries( ll::intel_memops ${INTEL_MEMOPS_LIBRARY} )
    endif (INTEL_MEMOPS_LIBRARY)

    if (ADDRESS_SIZE EQUAL 64)
        # We deliberately omit this from the 32bit build because it declares that
        # the viewer is compatible with Windows 10; we need that to properly detect
        # the Windows version, but doing so causes systems with certain HD video
        # cards to fail because Windows 10 does not support them.  Leaving this out
        # causes those systems to run in a Windows 8 compatibility mode, which works.
        LIST(APPEND viewer_SOURCE_FILES windows.manifest)
    endif (ADDRESS_SIZE EQUAL 64)

endif (WINDOWS)

# Add the xui files. This is handy for searching for xui elements
# from within the IDE.
file(GLOB_RECURSE viewer_XUI_FILES LIST_DIRECTORIES FALSE
    ${CMAKE_CURRENT_SOURCE_DIR}/skins/*.xml)
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/skins PREFIX "XUI Files" FILES ${viewer_XUI_FILES})
set_source_files_properties(${viewer_XUI_FILES}
                            PROPERTIES HEADER_FILE_ONLY TRUE)
list(APPEND viewer_SOURCE_FILES ${viewer_XUI_FILES})


set(viewer_APPSETTINGS_FILES
    app_settings/anim.ini
    app_settings/autoreplace.xml
    app_settings/cmd_line.xml
    app_settings/commands.xml
    app_settings/emoji_groups.xml
    app_settings/foldertypes.xml
    app_settings/grass.xml
    app_settings/ignorable_dialogs.xml
    app_settings/key_bindings.xml
    app_settings/keywords_lsl_default.xml
    app_settings/keywords_lsl_preproc.xml
    app_settings/logcontrol.xml
    app_settings/settings.xml
    app_settings/settings_crash_behavior.xml
    app_settings/settings_files.xml
    app_settings/settings_per_account.xml
    app_settings/settings_renames.xml
    app_settings/std_bump.ini
    app_settings/toolbars.xml
    app_settings/trees.xml
    app_settings/viewerart.xml
    app_settings/viewer_import_map.xml
    app_settings/viewer_presets/alchemy.xml
    app_settings/viewer_presets/blackdragon.xml
    app_settings/viewer_presets/firestorm.xml
    app_settings/viewer_presets/secondlife.xml
    app_settings/viewer_presets/slipstream.xml
    app_settings/gamecontrollerdb.txt
    app_settings/message.xml
    ${CMAKE_SOURCE_DIR}/../scripts/messages/message_template.msg
    packages-info.txt
    featuretable.txt
    featuretable_mac.txt
    featuretable_linux.txt
    )

source_group("App Settings" FILES ${viewer_APPSETTINGS_FILES})

set_source_files_properties(${viewer_APPSETTINGS_FILES}
                            PROPERTIES HEADER_FILE_ONLY TRUE)

list(APPEND viewer_SOURCE_FILES ${viewer_APPSETTINGS_FILES})

set(viewer_CHARACTER_FILES
    character/attentions.xml
    character/attentionsN.xml
    character/avatar_lad.xml
    character/avatar_skeleton.xml
    character/genepool.xml
    )

source_group("Character File" FILES ${viewer_CHARACTER_FILES})

set_source_files_properties(${viewer_CHARACTER_FILES}
                            PROPERTIES HEADER_FILE_ONLY TRUE)
list(APPEND viewer_SOURCE_FILES ${viewer_CHARACTER_FILES})

set(viewer_MISC_FILES
    viewer_manifest.py
    )

set_source_files_properties(${viewer_MISC_FILES}
                            PROPERTIES HEADER_FILE_ONLY TRUE)

list(APPEND viewer_SOURCE_FILES ${viewer_MISC_FILES})

if (WINDOWS)
  file(GLOB viewer_INSTALLER_FILES installers/windows/*.iss)

  source_group("Installer Files" FILES ${viewer_INSTALLER_FILES})

  set_source_files_properties(${viewer_INSTALLER_FILES}
                              PROPERTIES HEADER_FILE_ONLY TRUE)

  list(APPEND viewer_SOURCE_FILES ${viewer_INSTALLER_FILES})
endif (WINDOWS)

list(APPEND viewer_SOURCE_FILES ${viewer_HEADER_FILES})

# Build commit for the startup log ("Build commit:"), so tester logs name the exact
# build. Only llappviewer.cpp gets the define (no project-wide rebuild on a new commit).
execute_process(COMMAND git rev-parse --short=10 HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE LL_BUILD_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if (NOT LL_BUILD_COMMIT)
  set(LL_BUILD_COMMIT "unknown")
endif ()
set_property(SOURCE llappviewer.cpp APPEND PROPERTY COMPILE_DEFINITIONS "LL_BUILD_COMMIT=\"${LL_BUILD_COMMIT}\"")

add_executable(${VIEWER_BINARY_NAME}
    WIN32
    MACOSX_BUNDLE
    ${viewer_SOURCE_FILES}
    )
ll_fix_isa_note(${VIEWER_BINARY_NAME})
add_dependencies(${VIEWER_BINARY_NAME} vk_shaders)
# <GMDX> The world SPIR-V arrays are defined once here (vk_world_shaders_spv.h only declares them).
set_source_files_properties("${VK_SHADER_GEN_DIR}/vk_world_shaders_spv.cpp" PROPERTIES
    GENERATED TRUE SKIP_PRECOMPILE_HEADERS ON)
target_sources(${VIEWER_BINARY_NAME} PRIVATE "${VK_SHADER_GEN_DIR}/vk_world_shaders_spv.cpp")
target_include_directories(${VIEWER_BINARY_NAME} PRIVATE "${VK_SHADER_GEN_DIR}")

# <GMDX> Crash reports (gmdxcrashreport.cpp): the Crashpad package scripts/crashpad/build.sh makes
# (build-crashpad-linux / build-crashpad-windows beside indra/). Without it the viewer builds
# without crash reports; with it, crashpad_handler ships beside the binary (viewer_manifest.py).
if (WINDOWS)
  set(_gmdx_crashpad_default "${CMAKE_SOURCE_DIR}/../build-crashpad-windows")
else ()
  set(_gmdx_crashpad_default "${CMAKE_SOURCE_DIR}/../build-crashpad-linux")
endif ()
set(GMDX_CRASHPAD_DIR "${_gmdx_crashpad_default}" CACHE PATH "Crashpad package (scripts/crashpad/build.sh)")
if (EXISTS "${GMDX_CRASHPAD_DIR}/lib" AND EXISTS "${GMDX_CRASHPAD_DIR}/include/client/crashpad_client.h")
  message(STATUS "GunMetal-DX crash reports: Crashpad from ${GMDX_CRASHPAD_DIR}")
  set_source_files_properties(gmdxcrashreport.cpp PROPERTIES
      COMPILE_DEFINITIONS "GMDX_CRASHPAD=1"
      INCLUDE_DIRECTORIES "${GMDX_CRASHPAD_DIR}/include"
      SKIP_PRECOMPILE_HEADERS ON)
  if (WINDOWS)
    target_link_libraries(${VIEWER_BINARY_NAME}
        "${GMDX_CRASHPAD_DIR}/lib/crashpad_client.lib" "${GMDX_CRASHPAD_DIR}/lib/crashpad_util.lib"
        "${GMDX_CRASHPAD_DIR}/lib/mini_chromium.lib" "${GMDX_CRASHPAD_DIR}/lib/crashpad_mpack.lib"
        "${GMDX_CRASHPAD_DIR}/lib/crashpad_compat.lib" advapi32 winhttp version)
  else ()
    target_link_libraries(${VIEWER_BINARY_NAME}
        "${GMDX_CRASHPAD_DIR}/lib/libcrashpad_client.a" "${GMDX_CRASHPAD_DIR}/lib/libcrashpad_util.a"
        "${GMDX_CRASHPAD_DIR}/lib/libmini_chromium.a" "${GMDX_CRASHPAD_DIR}/lib/libcrashpad_mpack.a"
        "${GMDX_CRASHPAD_DIR}/lib/libcrashpad_compat.a")
  endif ()
  # beside the build's viewer binary, where viewer_manifest.py picks it up
  if (WINDOWS)
    set(_gmdx_handler "${GMDX_CRASHPAD_DIR}/bin/crashpad_handler.exe")
  else ()
    set(_gmdx_handler "${GMDX_CRASHPAD_DIR}/bin/crashpad_handler")
  endif ()
  add_custom_command(TARGET ${VIEWER_BINARY_NAME} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_gmdx_handler}" "${CMAKE_CURRENT_BINARY_DIR}/")
  set(GMDX_CRASHPAD_HANDLER "${_gmdx_handler}" CACHE INTERNAL "")
else ()
  message(STATUS "GunMetal-DX crash reports: off (no Crashpad package at ${GMDX_CRASHPAD_DIR}; scripts/crashpad/build.sh)")
  set(GMDX_CRASHPAD_HANDLER "" CACHE INTERNAL "")
endif ()
# </GMDX>

# add package files
file(GLOB EVENT_HOST_SCRIPT_GLOB_LIST
     ${CMAKE_CURRENT_SOURCE_DIR}/../viewer_components/*.py)
list(APPEND EVENT_HOST_SCRIPTS ${EVENT_HOST_SCRIPT_GLOB_LIST})

set(PACKAGE ON CACHE BOOL
    "Add a package target that builds an installer package.")

if(USE_PRECOMPILED_HEADERS)
  target_precompile_headers( ${VIEWER_BINARY_NAME} PRIVATE llviewerprecompiledheaders.h )
endif(USE_PRECOMPILED_HEADERS)

if (WINDOWS)
    set_target_properties(${VIEWER_BINARY_NAME}
        PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" # Sets VS debugger working director
        # *TODO -reenable this once we get server usage sorted out
        LINK_FLAGS "/debug /NODEFAULTLIB:LIBCMT /SUBSYSTEM:WINDOWS /LARGEADDRESSAWARE"
        LINK_FLAGS_DEBUG "/NODEFAULTLIB:\"LIBCMT;LIBCMTD;MSVCRT\" /INCREMENTAL:NO /LARGEADDRESSAWARE"
        LINK_FLAGS_RELEASE "/FORCE:MULTIPLE /MAP:\"slipstream-bin.MAP\" /OPT:REF /LARGEADDRESSAWARE"
        )
    target_compile_options(${VIEWER_BINARY_NAME} PRIVATE /bigobj)

    # If adding a file to viewer_manifest.py in the WindowsManifest.construct() method, be sure to add the dependency
    # here.
    # *NOTE:Mani - This is a crappy hack to have important dependencies for the viewer_manifest copy action
    # be met. I'm looking forward to a source-code split-up project next year that will address this kind of thing.
    # In the meantime, if you have any ideas on how to easily maintain one list, either here or in viewer_manifest.py
    # and have the build deps get tracked *please* tell me about it.
    # nat: https://cmake.org/cmake/help/v3.14/command/file.html
    # "For example, the code
    # file(STRINGS myfile.txt myfile)
    # stores a list in the variable myfile in which each item is a line from the input file."
    # And of course it's straightforward to read a text file in Python.

    set(COPY_INPUT_DEPENDENCIES
      # The following commented dependencies are determined variably at build time. Can't do this here.
      app_settings/message.xml
      ${CMAKE_SOURCE_DIR}/../scripts/messages/message_template.msg
      ${SHARED_LIB_STAGING_DIR}/openjp2.dll
      ${SHARED_LIB_STAGING_DIR}/llwebrtc.dll
      ${PREBUILT_INSTALL_DIR}/ca-bundle.crt
      ${CMAKE_CURRENT_SOURCE_DIR}/licenses-win32.txt
      ${CMAKE_CURRENT_SOURCE_DIR}/featuretable.txt
      ${viewer_APPSETTINGS_FILES}
      SLPlugin
      media_plugin_cef
      media_plugin_libvlc
      media_plugin_example
      )

    if (TARGET ll::openal)
      list(APPEND COPY_INPUT_DEPENDENCIES
           ${SHARED_LIB_STAGING_DIR}/openal32.dll
           ${SHARED_LIB_STAGING_DIR}/alut.dll
          )
    endif ()

    add_custom_command(
      OUTPUT  ${CMAKE_CFG_INTDIR}/copy_touched.bat
      COMMAND ${PYTHON_EXECUTABLE}
      ARGS
        ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
        --actions=copy
        --arch=${ARCH}
        --platform=windows
        --artwork=${ARTWORK_DIR}
        "--openal=${USE_OPENAL}"
        "--tracy=${USE_TRACY}"
        --build=${CMAKE_CURRENT_BINARY_DIR}
        --buildtype=$<CONFIG>
        "--channel=${VIEWER_CHANNEL}"
        --configuration=${CMAKE_CFG_INTDIR}
        --dest=${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>
        --grid=${GRID}
        --source=${CMAKE_CURRENT_SOURCE_DIR}
        --touch=${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>/copy_touched.bat
        --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
        ${CMAKE_CURRENT_SOURCE_DIR}/vlc_plugin_allowlist.txt
        stage_third_party_libs
        llwebrtc
        ${COPY_INPUT_DEPENDENCIES}
      COMMENT "Performing viewer_manifest copy"
      )

    add_custom_target(copy_w_viewer_manifest ALL DEPENDS ${CMAKE_CFG_INTDIR}/copy_touched.bat)

    set_target_properties(copy_w_viewer_manifest
            PROPERTIES
            FOLDER "BuildUtils"
            )

    add_dependencies(${VIEWER_BINARY_NAME} stage_third_party_libs llcommon llwebrtc copy_w_viewer_manifest)

    # Useful for remote debugging or profiling if set to target a shared drive. for example, brad sets it to "dist/secondlife" to deploy under
    # the build tree (and then I share that directory) or "f:/dist/secondlife" to deploy to a network mounted drive (although this can be slow).
    # this will also create an "unpacked" tarball at build-<platform>/newview/unpacked_<platform>_7_1_11_<build num>.tar
    set(LOCAL_DIST_DIR "" CACHE PATH "Path for 'local' unpacked copy of viewer distribution to be deployed to.")
    set(product SecondLife-${ARCH}-${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION})
    if(IS_DIRECTORY ${LOCAL_DIST_DIR})
        add_custom_command(
            OUTPUT  ${LOCAL_DIST_DIR}/.${product}.copy_touched
            COMMAND ${PYTHON_EXECUTABLE}
            ARGS
            ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
            "--actions=\"copy package unpacked\""
            --arch=${ARCH}
            --platform=windows
            --artwork=${ARTWORK_DIR}
            "--openal=${USE_OPENAL}"
            "--tracy=${USE_TRACY}"
            --build=${CMAKE_CURRENT_BINARY_DIR}
            --buildtype=$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,${CMAKE_BUILD_TYPE}>
            "--channel=${VIEWER_CHANNEL}"
            --configuration=$<CONFIG>
            --dest=${LOCAL_DIST_DIR}
            --grid=${GRID}
            --source=${CMAKE_CURRENT_SOURCE_DIR}
            --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
            --touch=${LOCAL_DIST_DIR}/.${product}.copy_touched
            DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
            stage_third_party_libs
            llwebrtc
            ${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
            ${COPY_INPUT_DEPENDENCIES}
            ${VIEWER_BINARY_NAME}
            COMMENT "Performing viewer_manifest local dist copy"
            )

        add_custom_target(copy_local_dist_viewer_manifest ALL DEPENDS ${LOCAL_DIST_DIR}/.${product}.copy_touched)
    elseif (NOT LOCAL_DIST_DIR STREQUAL "")
        message(FATAL_ERROR "LOCAL_DIST_DIR ${LOCAL_DIST_DIR} specified but is not valid target directory to copy viewer distribution into.  Please create the directory and try again")
    endif ()  # IS_DIRECTORY ${LOCAL_DIST_DIR}

    if (EXISTS ${CMAKE_SOURCE_DIR}/copy_win_scripts)
      add_dependencies(${VIEWER_BINARY_NAME} copy_win_scripts)
    endif (EXISTS ${CMAKE_SOURCE_DIR}/copy_win_scripts)

    add_dependencies(${VIEWER_BINARY_NAME} SLPlugin)

    if (PACKAGE)
      add_custom_command(
        OUTPUT ${CMAKE_CFG_INTDIR}/touched.bat
        COMMAND ${PYTHON_EXECUTABLE}
        ARGS
              ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
              --arch=${ARCH}
              --platform=windows
              --artwork=${ARTWORK_DIR}
              "--openal=${USE_OPENAL}"
              "--tracy=${USE_TRACY}"
              --build=${CMAKE_CURRENT_BINARY_DIR}
              --buildtype=$<CONFIG>
              "--channel=${VIEWER_CHANNEL}"
              --configuration=${CMAKE_CFG_INTDIR}
              --dest=${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>
              --grid=${GRID}
              --source=${CMAKE_CURRENT_SOURCE_DIR}
              --touch=${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>/touched.bat
              --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
        DEPENDS
            ${VIEWER_BINARY_NAME}
            ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
            ${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
            ${COPY_INPUT_DEPENDENCIES}
        )

      add_custom_target(llpackage ALL DEPENDS
              ${CMAKE_CFG_INTDIR}/touched.bat
        )

    endif (PACKAGE)
elseif (DARWIN)
    set_target_properties(${VIEWER_BINARY_NAME}
        PROPERTIES
        RESOURCE SecondLife.xib
        LINK_FLAGS_RELEASE "${LINK_FLAGS_RELEASE} -Xlinker -dead_strip"
        )
else (WINDOWS)
        # Linux
    set_target_properties(${VIEWER_BINARY_NAME}
        PROPERTIES
        LINK_FLAGS_RELEASE "${LINK_FLAGS_RELEASE} -Wl,--Map=${VIEWER_BINARY_NAME}.MAP"
        )
endif (WINDOWS)

# *NOTE: - this list is very sensitive to ordering, test carefully on all
# platforms if you change the relative order of the entries here.
# In particular, cmake 2.6.4 (when building with linux/makefile generators)
# appears to sometimes de-duplicate redundantly listed dependencies improperly.
# To work around this, higher level modules should be listed before the modules
# that they depend upon. -brad
#
# *NOTE:  On mixing system shared libraries and updated static archives.
# We use a number of libraries that have an existence as system libraries,
# internal-use libraries and applications libraries.  The most-referenced
# one of these being libz where you can find four or more versions in play
# at once.  On Linux, libz can be found at link and run time via a number
# of paths:
#
#      => -lfreetype
#        => libz.so.1 (on install machine, not build)
#      => -lSDL
#        => libz.so.1 (on install machine, not build)
#      => -lgdk-x11-2.0
#        => libz.so.1
#      => -lz
#
# We generally want the newest version of the library to provide all symbol
# resolution.  To that end, when using static archives, the *_PRELOAD_ARCHIVES
# variables, PNG_PRELOAD_ARCHIVES and ZLIBNG_PRELOAD_ARCHIVES, get the archives
# dumped into the target binary and runtime lookup will find the most
# modern version.

target_link_libraries(${VIEWER_BINARY_NAME}
        llaudio
        llcharacter
        llimage
        llinventory
        llmessage
        llplugin
        llprimitive
        llrender
        llui
        llfilesystem
        llcachestore # #715 segment store (slcachestore.cpp, llgputexcachestore.cpp)
        llwindow
        llxml
        llmath
        llcorehttp
        llcommon
        llmeshoptimizer
        llwebrtc
        lllogin
        llprimitive
        llappearance
        ${LLPHYSICSEXTENSIONS_LIBRARIES}
        ll::ndof
        ll::tracy
        ll::SDL3     # EXPERIMENTAL Vulkan: SDL_Vulkan_* surface creation
        ll::vulkan   # EXPERIMENTAL Vulkan: loader + headers for vkgfx
        slreshadefx  # ReShade FX effect compiler (#543, vkgfx/third_party/reshadefx)
        ll::libwebp  # WebP decode + demux for chat image previews (#651, cmake/WebP.cmake)
        )


if( TARGET ll::intel_memops )
   target_link_libraries(${VIEWER_BINARY_NAME} ll::intel_memops )
endif()

if( TARGET ll::nvapi )
   target_link_libraries(${VIEWER_BINARY_NAME} ll::nvapi )
endif()

if ( TARGET llconvexdecomposition )
    target_link_libraries(${VIEWER_BINARY_NAME} llconvexdecomposition )
endif ()

set(ARTWORK_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE PATH
    "Path to artwork files.")

message("Copying fonts")
file(GLOB FONT_FILE_GLOB_LIST
  "${PREBUILT_INSTALL_DIR}/fonts/*"
)
file(COPY ${FONT_FILE_GLOB_LIST} DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/fonts")

# Copy over the Emoji/shortcodes mapping XML files (and create dependency
# if they are changed, CMake will run again and copy over new versions)
message("Copying Emoji/shortcode mappings")
set(emoji_mapping_src_folder ${PREBUILT_INSTALL_DIR}/xui)
set(emoji_mapping_dst_folder ${CMAKE_CURRENT_SOURCE_DIR}/skins/default/xui)

# Note Turkey is missing from this set (not available in Emoji package yet)
set(country_codes "da;de;en;es;fr;it;ja;pl;pt;ru;zh")
foreach(elem ${country_codes})
   set(emoji_mapping_src_file
      "${emoji_mapping_src_folder}/${elem}/emoji_characters.xml")
   set(emoji_mapping_dst_file
      "${emoji_mapping_dst_folder}/${elem}/emoji_characters.xml")
   configure_file(${emoji_mapping_src_file} ${emoji_mapping_dst_file} COPYONLY)
endforeach()

if (LINUX)
  set(product SecondLife-${ARCH}-${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION})

  # These are the generated targets that are copied to package/
  set(COPY_INPUT_DEPENDENCIES
    ${VIEWER_BINARY_NAME}
    SLPlugin
    media_plugin_cef
    media_plugin_gstreamer10
    media_plugin_libvlc
    media_plugin_example
    llwebrtc
    )

  add_custom_command(
      OUTPUT ${product}.tar.xz
      COMMAND ${PYTHON_EXECUTABLE}
      ARGS
        ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
        --arch=${ARCH}
        --artwork=${ARTWORK_DIR}
        "--openal=${USE_OPENAL}"
        "--tracy=${USE_TRACY}"
        --build=${CMAKE_CURRENT_BINARY_DIR}
        --buildtype=${CMAKE_BUILD_TYPE}
        "--channel=${VIEWER_CHANNEL}"
        --configuration=${CMAKE_CFG_INTDIR}
        --dest=${CMAKE_CURRENT_BINARY_DIR}/packaged
        --grid=${GRID}
        --source=${CMAKE_CURRENT_SOURCE_DIR}
        --touch=${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/.${product}.touched
        --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
      DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
        ${COPY_INPUT_DEPENDENCIES}
      )

  if (PACKAGE)
  endif (PACKAGE)

  add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/.${product}.copy_touched
    COMMAND ${PYTHON_EXECUTABLE}
    ARGS
      ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
      --actions=copy
      --arch=${ARCH}
      --artwork=${ARTWORK_DIR}
      "--openal=${USE_OPENAL}"
      "--tracy=${USE_TRACY}"
      --build=${CMAKE_CURRENT_BINARY_DIR}
      --buildtype=${CMAKE_BUILD_TYPE}
      "--channel=${VIEWER_CHANNEL}"
      --configuration=${CMAKE_CFG_INTDIR}
      --dest=${CMAKE_CURRENT_BINARY_DIR}/packaged
      --grid=${GRID}
      --source=${CMAKE_CURRENT_SOURCE_DIR}
      --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
    DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
      ${COPY_INPUT_DEPENDENCIES}
    COMMENT "Performing viewer_manifest copy"
    )

  add_custom_target(copy_l_viewer_manifest ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/.${product}.copy_touched)

  if (PACKAGE)
    add_custom_target(llpackage ALL DEPENDS ${product}.tar.xz)
    # Make sure we don't run two instances of viewer_manifest.py at the same time.
    add_dependencies(llpackage copy_l_viewer_manifest)
    check_message_template(llpackage)
  endif (PACKAGE)
endif (LINUX)

if (DARWIN)
  # These all get set with PROPERTIES. It's not that the property names are
  # magically known to CMake -- it's that these names are referenced in the
  # Info-SecondLife.plist file in the configure_file() directive below.
  set(product "${VIEWER_CHANNEL}")
  set(MACOSX_EXECUTABLE_NAME "${VIEWER_CHANNEL}")
  set(MACOSX_BUNDLE_INFO_STRING "${VIEWER_CHANNEL}")
  set(MACOSX_BUNDLE_ICON_FILE "slipstream.icns")
  set(MACOSX_BUNDLE_GUI_IDENTIFIER "org.slipstreamviewer.viewer")
  set(MACOSX_BUNDLE_LONG_VERSION_STRING "${VIEWER_CHANNEL} ${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION}")
  set(MACOSX_BUNDLE_BUNDLE_NAME "Slipstream")
  set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${VIEWER_SHORT_VERSION}.${VIEWER_VERSION_REVISION}")
  set(MACOSX_BUNDLE_BUNDLE_VERSION "${VIEWER_SHORT_VERSION}${VIEWER_MACOSX_PHASE}${VIEWER_REVISION}")
  set(MACOSX_BUNDLE_COPYRIGHT "Copyright (C) 2013-2025 Alchemy Development Group")
  set(MACOSX_BUNDLE_NSMAIN_NIB_FILE "SecondLife.nib")
  set(MACOSX_BUNDLE_NSPRINCIPAL_CLASS "LLApplication")

  # https://blog.kitware.com/upcoming-in-cmake-2-8-12-osx-rpath-support/
  set(CMAKE_MACOSX_RPATH 1)

  set_target_properties(
    ${VIEWER_BINARY_NAME}
    PROPERTIES
    OUTPUT_NAME "${product}"
    # From Contents/MacOS/SecondLife, look in Contents/Frameworks
    BUILD_WITH_INSTALL_RPATH 1
    INSTALL_RPATH "@executable_path/../Frameworks"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info-SecondLife.plist"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${MACOSX_BUNDLE_GUI_IDENTIFIER}"
    )

  set(VIEWER_APP_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>/${product}.app")
  set(VIEWER_APP_EXE "${VIEWER_APP_BUNDLE}/Contents/MacOS/${product}")

  configure_file(
     "${CMAKE_CURRENT_SOURCE_DIR}/Info-SecondLife.plist"
     "${VIEWER_APP_BUNDLE}/Contents/Info.plist"
    )

  add_custom_command(
    TARGET ${VIEWER_BINARY_NAME} POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE}
    ARGS
      ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
      --actions=copy
      --arch=${ARCH}
      --artwork=${ARTWORK_DIR}
      "--openal=${USE_OPENAL}"
      "--tracy=${USE_TRACY}"
      --build=${CMAKE_CURRENT_BINARY_DIR}
      --buildtype=$<CONFIG>
      --bundleid=${MACOSX_BUNDLE_GUI_IDENTIFIER}
      "--channel=${VIEWER_CHANNEL}"
      --configuration=${CMAKE_CFG_INTDIR}
      --dest=${VIEWER_APP_BUNDLE}
      --grid=${GRID}
      --source=${CMAKE_CURRENT_SOURCE_DIR}
      --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
    )

  add_dependencies(${VIEWER_BINARY_NAME} SLPlugin media_plugin_libvlc media_plugin_cef)

  if (ENABLE_SIGNING)
      set(SIGNING_SETTING "--signature=${SIGNING_IDENTITY}")
  else (ENABLE_SIGNING)
      set(SIGNING_SETTING "")
  endif (ENABLE_SIGNING)

  if (PACKAGE)
      add_custom_target(llpackage ALL DEPENDS ${VIEWER_BINARY_NAME})

      add_custom_command(
        TARGET llpackage POST_BUILD
        COMMAND ${PYTHON_EXECUTABLE}
        ARGS
              ${CMAKE_CURRENT_SOURCE_DIR}/viewer_manifest.py
              --arch=${ARCH}
              --artwork=${ARTWORK_DIR}
              "--openal=${USE_OPENAL}"
              "--tracy=${USE_TRACY}"
              --build=${CMAKE_CURRENT_BINARY_DIR}
              --buildtype=$<CONFIG>
              "--channel=${VIEWER_CHANNEL}"
              --configuration=${CMAKE_CFG_INTDIR}
              --dest=${VIEWER_APP_BUNDLE}
              --grid=${GRID}
              --source=${CMAKE_CURRENT_SOURCE_DIR}
              --touch=${CMAKE_CURRENT_BINARY_DIR}/$<IF:$<BOOL:${LL_GENERATOR_IS_MULTI_CONFIG}>,$<CONFIG>,>/.${product}.bat
              --versionfile=${CMAKE_CURRENT_BINARY_DIR}/viewer_version.txt
              ${SIGNING_SETTING}
      )
  endif (PACKAGE)
endif (DARWIN)

if (INSTALL)
  include(${CMAKE_CURRENT_SOURCE_DIR}/ViewerInstall.cmake)
endif (INSTALL)

if (LL_TESTS)
  # To add a viewer unit test, just add the test .cpp file below
  # This creates a separate test project per file listed.
  include(LLAddBuildTest)
  SET(viewer_TEST_SOURCE_FILES
    llagentaccess.cpp
    lldateutil.cpp
#    llmediadataclient.cpp
    lllogininstance.cpp
#    llremoteparcelrequest.cpp
    llviewerhelputil.cpp
    llversioninfo.cpp
#    llvocache.cpp
    llworldmap.cpp
    llworldmipmap.cpp
  )

  set_source_files_properties(
    llworldmap.cpp
    llworldmipmap.cpp
    PROPERTIES
    LL_TEST_ADDITIONAL_SOURCE_FILES
    tests/llviewertexture_stub.cpp
    #llviewertexturelist.cpp
  )

#  set_source_files_properties(
#    llvocache.cpp
#    PROPERTIES
#    LL_TEST_ADDITIONAL_SOURCE_FILES ../llmessage/lldatapacker.cpp
#    LL_TEST_ADDITIONAL_PROJECTS "llprimitive"
#  )

  set(test_libs
          llcommon
          llfilesystem
          llxml
          llmessage
          llcharacter
          llui
          lllogin
          llplugin
          llappearance
    )

  set_source_files_properties(
    llmediadataclient.cpp
    PROPERTIES
    LL_TEST_ADDITIONAL_LIBRARIES "${test_libs}"
  )

  set_source_files_properties(
    llworldmap.cpp
    llworldmipmap.cpp
    PROPERTIES
    LL_TEST_ADDITIONAL_SOURCE_FILES
    tests/llviewertexture_stub.cpp
  )

  set_source_files_properties(
    llmediadataclient.cpp
    PROPERTIES
    LL_TEST_ADDITIONAL_LIBRARIES llprimitive
  )

  set_source_files_properties(
    lllogininstance.cpp
    PROPERTIES
    LL_TEST_ADDITIONAL_SOURCE_FILES llversioninfo.cpp
  )

  set_property( SOURCE
          ${viewer_TEST_SOURCE_FILES}
          PROPERTY
          LL_TEST_ADDITIONAL_LIBRARIES ${test_libs}
  )

  # LL_ADD_PROJECT_UNIT_TESTS(${VIEWER_BINARY_NAME} "${viewer_TEST_SOURCE_FILES}")

  #set(TEST_DEBUG on)

  set(test_libs
          llfilesystem
          llmath
          llcommon
          llmessage
          llcorehttp
          llxml
          llui
          llplugin
          llappearance
          lllogin
          llprimitive
          lllogin
          )

  LL_ADD_INTEGRATION_TEST(cppfeatures
    ""
    "${test_libs}"
    )

  LL_ADD_INTEGRATION_TEST(llsechandler_basic
    "llsechandler_basic.cpp;slrevocation.cpp"
    "${test_libs}"
    )

  LL_ADD_INTEGRATION_TEST(llsecapi
     llsecapi.cpp
    "${test_libs}"
    )

  set(llslurl_test_sources
      llslurl.cpp
      llviewernetwork.cpp
  )

  LL_ADD_INTEGRATION_TEST(llslurl
     "${llslurl_test_sources}"
    "${test_libs}"
    )

  set(llviewercontrollistener_test_sources
    llviewercontrollistener.cpp
    ../llxml/llcontrol.cpp
    ../llxml/llxmltree.cpp
    ../llxml/llxmlparser.cpp
    ../llcommon/commoncontrol.cpp
    )

  LL_ADD_INTEGRATION_TEST(llviewercontrollistener
    "${llviewercontrollistener_test_sources}"
    "${test_libs}"
    )

  LL_ADD_INTEGRATION_TEST(llviewernetwork
     llviewernetwork.cpp
    "${test_libs}"
    )

  # #714: the .gtx -> cache store converter over a temp folder (no viewer needed).
  LL_ADD_INTEGRATION_TEST(slgputexmigrate
    "slgputexmigrate.cpp;llgputexrecord.cpp"
    "llcachestore;llcommon"
    )

  # #865: GPU-ready per-level records (chain lookup, partial reads, corruption of one
  # level, the v1 -> v2 rewrite) over a temp store.
  LL_ADD_INTEGRATION_TEST(llgputexrecord
    llgputexrecord.cpp
    "llcachestore;llcommon"
    )

  # #714: the legacy J2C cache -> cache store converter over a temp folder (no viewer).
  LL_ADD_INTEGRATION_TEST(slj2cmigrate
    slj2cmigrate.cpp
    "llcachestore;llcommon"
    )

  # #714 / #715: the old asset folder -> cache store converter (mesh part split, resume).
  LL_ADD_INTEGRATION_TEST(slassetmigrate
    "slassetmigrate.cpp;slcachemigrate.cpp"
    "llcachestore;llcommon"
    )

  LL_ADD_INTEGRATION_TEST(llviewerassetstats
    llviewerassetstats.cpp
    "${test_libs}"
    )

# LL_ADD_INTEGRATION_TEST(llhttpretrypolicy "llhttpretrypolicy.cpp" "${test_libs}")

  #ADD_VIEWER_BUILD_TEST(llmemoryview viewer)
  #ADD_VIEWER_BUILD_TEST(llagentaccess viewer)
  #ADD_VIEWER_BUILD_TEST(lltextureinfo viewer)
  #ADD_VIEWER_BUILD_TEST(lltextureinfodetails viewer)


endif (LL_TESTS)

check_message_template(${VIEWER_BINARY_NAME})

