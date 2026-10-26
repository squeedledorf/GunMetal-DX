#!/usr/bin/env python3
"""\
@file viewer_manifest.py
@author Ryan Williams
@brief Description of all installer viewer files, and methods for packaging
       them into installers for all supported platforms.

$LicenseInfo:firstyear=2006&license=viewerlgpl$
Second Life Viewer Source Code
Copyright (C) 2006-2014, Linden Research, Inc.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation;
version 2.1 of the License only.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
$/LicenseInfo$
"""
import errno
import filecmp
import glob
import itertools
import json
import os
import os.path
import plistlib
import random
import re
import secrets
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

viewer_dir = os.path.dirname(__file__)
# Add indra/lib/python to our path so we don't have to muck with PYTHONPATH.
# Put it FIRST because some of our build hosts have an ancient install of
# indra.util.llmanifest under their system Python!
sys.path.insert(0, os.path.join(viewer_dir, os.pardir, "lib", "python"))
from indra.util.llmanifest import LLManifest, main, path_ancestors, CHANNEL_VENDOR_BASE, RELEASE_CHANNEL, ManifestError, MissingError
import llsd

class ViewerManifest(LLManifest):
    def is_packaging_viewer(self):
        # Some commands, files will only be included
        # if we are packaging the viewer on windows.
        # This manifest is also used to copy
        # files during the build (see copy_w_viewer_manifest
        # and copy_l_viewer_manifest targets)
        return 'package' in self.args['actions']

    def construct(self):
        super(ViewerManifest, self).construct()
        self.path(src="../../scripts/messages/message_template.msg", dst="app_settings/message_template.msg")

        os.environ["XZ_DEFAULTS"] = "-T0"

        if self.is_packaging_viewer():
            with self.prefix(src_dst="app_settings"):
                self.exclude("logcontrol.xml")
                self.exclude("logcontrol-dev.xml")
                self.path("*.ini")
                self.path("*.txt")
                self.path("*.xml")

                # include the extracted list of contributors
                contributions_path = os.path.join(self.args['source'], "..", "..", "doc", "contributions.txt")
                contributor_names = self.extract_names(contributions_path)
                self.put_in_file(contributor_names.encode(), "contributors.txt", src=contributions_path)

                # include the extracted list of supporters
                supporters_path = os.path.join(self.args['source'], "..", "..", "doc", "supporters.txt")
                supporters_names = self.extract_names(supporters_path)
                self.put_in_file(supporters_names.encode(), "supporters.txt", src=supporters_path)

                # <GMDX> the volumetric clouds' cloud bodies (CorvaeOboro's CC0 volumes,
                # scripts/clouds/corvae_shapes.py)
                self.path("gmdx_cloud_shapes.bin")

                # ... and the default camera position settings
                self.path("camera")

                # ... and the entire windlight directory
                self.path("windlight")

                # ... and the built-in effects stack headers (ReShade.fxh / ReShadeUI.fxh, #543)
                self.path("fx")

                # ... and the weather sound pack (#783, CC0; see sounds/weather/SOURCES.md)
                self.path("sounds")

                # ... and the entire image filters directory
                self.path("filters")
                self.path("mesh_upload")

                # <FS:AR> Poser Presets
                self.path("poses")

                # ... and the Pose Studio bone picker figures (#500)
                self.path("posestudio")

                # ... and the #Slipstream Helper bridge script (#500)
                self.path("slbridge")

                # ... and the parcel stage speaker setup scripts (#892)
                self.path("slspeakers")

                # ... and the "which viewer did you use before?" presets (#132)
                self.path("viewer_presets")

                # ... and the certificate revocation data: static distrust list + baseline
                # Chrome CRLSet (slrevocationupdater.cpp; release CI refreshes crl-set)
                self.path("revocation")

                # ... and the included spell checking dictionaries
                pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
                with self.prefix(src=pkgdir):
                    self.path("dictionaries")

                # include the extracted packages information (see BuildPackagesInfo.cmake)
                self.path(src=os.path.join(self.args['build'],"packages-info.txt"), dst="packages-info.txt")
                # CHOP-955: If we have "sourceid" or "viewer_channel" in the
                # build process environment, generate it into
                # settings_install.xml.
                settings_template = dict(
                    sourceid=dict(Comment='Identify referring agency to Linden web servers',
                                  Persist=1,
                                  Type='String',
                                  Value=''),
                    CmdLineGridChoice=dict(Comment='Default grid',
                                  Persist=0,
                                  Type='String',
                                  Value=''),
                    CmdLineChannel=dict(Comment='Command line specified channel name',
                                        Persist=0,
                                        Type='String',
                                        Value=''))
                settings_install = {}
                sourceid = self.args.get('sourceid')
                if sourceid:
                    settings_install['sourceid'] = settings_template['sourceid'].copy()
                    settings_install['sourceid']['Value'] = sourceid
                    print("Set sourceid in settings_install.xml to '%s'" % sourceid)

                if self.args.get('channel_suffix'):
                    settings_install['CmdLineChannel'] = settings_template['CmdLineChannel'].copy()
                    settings_install['CmdLineChannel']['Value'] = self.channel_with_pkg_suffix()
                    print("Set CmdLineChannel in settings_install.xml to '%s'" % self.channel_with_pkg_suffix())

                if self.args.get('grid'):
                    settings_install['CmdLineGridChoice'] = settings_template['CmdLineGridChoice'].copy()
                    settings_install['CmdLineGridChoice']['Value'] = self.grid()
                    print("Set CmdLineGridChoice in settings_install.xml to '%s'" % self.grid())

                # put_in_file(src=) need not be an actual pathname; it
                # only needs to be non-empty
                self.put_in_file(llsd.format_pretty_xml(settings_install),
                                 "settings_install.xml",
                                 src="environment")


            with self.prefix(src_dst="character"):
                self.path("*.llm")
                self.path("*.xml")
                self.path("*.tga")

            # Include our fonts
            with self.prefix(src=os.path.join(pkgdir, 'fonts'), dst="fonts"):
                self.path("*.ttf")
                self.path("*.txt")
            # Selectable font sets (Preferences > Advanced > Font; FSFontSettingsFile):
            # the fonts_*.xml presets, their TTFs and licence files, beside the defaults.
            with self.prefix(src="fontsets", dst="fonts"):
                self.path("*.ttf")
                self.path("*.xml")
                self.path("*.txt")

            # skins
            with self.prefix(src_dst="skins"):
                # include the entire textures directory recursively
                with self.prefix(src_dst="*/textures"):
                    self.path("*/*.jpg")
                    self.path("*/*.png")
                    # #1056: Phosphor vector icons (vector_file in textures.xml) + their licence
                    self.path("phosphor/*.svg")
                    self.path("phosphor/LICENSE")
                    self.path("phosphor_authored/*.svg")   # our own, in the same style
                    self.path("salt_and_pepper.jpg")
                    self.path("*.tga")
                    self.path("*.j2c")
                    self.path("*.png")
                    self.path("textures.xml")
                self.path("*/xui/*/*.xml")
                self.path("*/xui/*/widgets/*.xml")
                self.path("*/*.xml")
                self.path("*/*.json")

                # Update: 2017-11-01 CP Now we store app code in the html folder
                #         Initially the HTML/JS code to render equirectangular
                #         images for the 360 capture feature but more to follow.
                with self.prefix(src="*/html", dst="*/html"):
                    self.path("*/*/*/*.js")
                    self.path("*/*/*.html")

            #build_data.json.  Standard with exception handling is fine.  If we can't open a new file for writing, we have worse problems
            #platform is computed above with other arg parsing
            build_data_dict = {"Type":"viewer","Version":'.'.join(self.args['version']),
                            "Channel Base": CHANNEL_VENDOR_BASE,
                            "Channel":self.channel_with_pkg_suffix(),
                            "Platform":self.build_data_json_platform,
                            "Address Size":self.address_size,
                            "Update Service":"https://update.secondlife.com/update",
                            }
            build_data_dict = self.finish_build_data_dict(build_data_dict)
            with open(os.path.join(os.pardir,'build_data.json'), 'w') as build_data_handle:
                json.dump(build_data_dict,build_data_handle)

            #we likely no longer need the test, since we will throw an exception above, but belt and suspenders and we get the
            #return code for free.
            if not self.path2basename(os.pardir, "build_data.json"):
                print("No build_data.json file")

    def finish_build_data_dict(self, build_data_dict):
        return build_data_dict

    def grid(self):
        return self.args['grid']

    def channel(self):
        return self.args['channel']

    def channel_with_pkg_suffix(self):
        fullchannel=self.channel()
        channel_suffix = self.args.get('channel_suffix')
        if channel_suffix:
            fullchannel+=' '+channel_suffix
        return fullchannel

    def channel_variant(self):
        global CHANNEL_VENDOR_BASE
        return self.channel().replace(CHANNEL_VENDOR_BASE, "").strip()

    def channel_type(self): # returns 'release', 'beta', 'project', or 'test'
        channel_qualifier=self.channel_variant().lower()
        if channel_qualifier.startswith('release'):
            channel_type='release'
        elif channel_qualifier.startswith('beta'):
            channel_type='beta'
        elif channel_qualifier.startswith('project'):
            channel_type='project'
        else:
            channel_type='test'
        return channel_type

    def channel_variant_app_suffix(self):
        # get any part of the channel name after the CHANNEL_VENDOR_BASE
        suffix=self.channel_variant()
        # by ancient convention, we don't use Release in the app name
        if self.channel_type() == 'release':
            suffix=suffix.replace('Release', '').strip()
        # for the base release viewer, suffix will now be null - for any other, append what remains
        if suffix:
            suffix = "_".join([''] + suffix.split())
        # the additional_packages mechanism adds more to the installer name (but not to the app name itself)
        # ''.split() produces empty list, so suffix only changes if
        # channel_suffix is non-empty
        suffix = "_".join([suffix] + self.args.get('channel_suffix', '').split())
        return suffix

    def installer_base_name(self):
        global CHANNEL_VENDOR_BASE
        # a standard map of strings for replacing in the templates
        substitution_strings = {
            'channel_vendor_base' : '_'.join(CHANNEL_VENDOR_BASE.split()),
            'channel_variant_underscores':self.channel_variant_app_suffix(),
            'version_underscores' : '_'.join(self.args['version']),
            'arch':self.args['arch']
            }
        return "%(channel_vendor_base)s%(channel_variant_underscores)s_%(version_underscores)s_%(arch)s" % substitution_strings

    def app_name(self):
        global CHANNEL_VENDOR_BASE
        channel_type=self.channel_type()
        if channel_type == 'release':
            app_suffix='Viewer'
        else:
            app_suffix=self.channel_variant()
        return CHANNEL_VENDOR_BASE + ' ' + app_suffix

    def exec_name(self):
        return "GunMetalDX"   # <GMDX>

    def app_name_oneword(self):
        return ''.join(self.app_name().split())

    def icon_path(self):
        # Slipstream: one in-tree icon set for every channel.
        return os.path.join(self.args['source'], 'icons', 'slipstream')

    def extract_names(self,src):
        """Extract contributor names from source file, returns string"""
        try:
            with open(src, 'r') as contrib_file:
                lines = contrib_file.readlines()
        except IOError:
            print("Failed to open '%s'" % src)
            raise

        # All lines up to and including the first blank line are the file header; skip them
        lines.reverse() # so that pop will pull from first to last line
        while not re.match(r"\s*$", lines.pop()) :
            pass # do nothing

        # A line that starts with a non-whitespace character is a name; all others describe contributions, so collect the names
        names = []
        for line in lines :
            if re.match(r"\S", line) :
                names.append(line.rstrip())
        # It's not fair to always put the same people at the head of the list
        random.shuffle(names)
        return ', '.join(names)

    def relsymlinkf(self, src, dst=None, catch=True):
        """
        relsymlinkf() is just like symlinkf(), but instead of requiring the
        caller to pass 'src' as a relative pathname, this method expects 'src'
        to be absolute, and creates a symlink whose target is the relative
        path from 'src' to dirname(dst).
        """
        dstdir, dst = self._symlinkf_prep_dst(src, dst)

        # Determine the relative path starting from the directory containing
        # dst to the intended src.
        src = self.relpath(src, dstdir)

        self._symlinkf(src, dst, catch)
        return dst

    def symlinkf(self, src, dst=None, catch=True):
        """
        Like ln -sf, but uses os.symlink() instead of running ln. This creates
        a symlink at 'dst' that points to 'src' -- see:
        https://docs.python.org/3/library/os.html#os.symlink

        If you omit 'dst', this creates a symlink with basename(src) at
        get_dst_prefix() -- in other words: put a symlink to this pathname
        here at the current dst prefix.

        'src' must specifically be a *relative* symlink. It makes no sense to
        create an absolute symlink pointing to some path on the build machine!

        Also:
        - We prepend 'dst' with the current get_dst_prefix(), so it has similar
          meaning to associated self.path() calls.
        - We ensure that the containing directory os.path.dirname(dst) exists
          before attempting the symlink.

        If you pass catch=False, exceptions will be propagated instead of
        caught.
        """
        dstdir, dst = self._symlinkf_prep_dst(src, dst)
        self._symlinkf(src, dst, catch)
        return dst

    def _symlinkf_prep_dst(self, src, dst):
        # helper for relsymlinkf() and symlinkf()
        if dst is None:
            dst = os.path.basename(src)
        dst = os.path.join(self.get_dst_prefix(), dst)
        # Seems silly to prepend get_dst_prefix() to dst only to call
        # os.path.dirname() on it again, but this works even when the passed
        # 'dst' is itself a pathname.
        dstdir = os.path.dirname(dst)
        self.cmakedirs(dstdir)
        return (dstdir, dst)

    def _symlinkf(self, src, dst, catch):
        # helper for relsymlinkf() and symlinkf()
        # the passed src must be relative
        if os.path.isabs(src):
            raise ManifestError("Do not symlinkf(absolute %r, asis=True)" % src)

        # The outer catch is the one that reports failure even after attempted
        # recovery.
        try:
            # At the inner layer, recovery may be possible.
            try:
                os.symlink(src, dst)
            except OSError as err:
                if err.errno != errno.EEXIST:
                    raise
                # We could just blithely attempt to remove and recreate the target
                # file, but that strategy doesn't work so well if we don't have
                # permissions to remove it. Check to see if it's already the
                # symlink we want, which is the usual reason for EEXIST.
                elif os.path.islink(dst):
                    if os.readlink(dst) == src:
                        # the requested link already exists
                        pass
                    else:
                        # dst is the wrong symlink; attempt to remove and recreate it
                        os.remove(dst)
                        os.symlink(src, dst)
                elif os.path.isdir(dst):
                    print("Requested symlink (%s) exists but is a directory; replacing" % dst)
                    shutil.rmtree(dst)
                    os.symlink(src, dst)
                elif os.path.exists(dst):
                    print("Requested symlink (%s) exists but is a file; replacing" % dst)
                    os.remove(dst)
                    os.symlink(src, dst)
                else:
                    # out of ideas
                    raise
        except Exception as err:
            # report
            print("Can't symlink %r -> %r: %s: %s" % \
                  (dst, src, err.__class__.__name__, err))
            # if caller asked us not to catch, re-raise this exception
            if not catch:
                raise

    def relpath(self, path, base=None, symlink=False):
        """
        Return the relative path from 'base' to the passed 'path'. If base is
        omitted, self.get_dst_prefix() is assumed. In other words: make a
        same-name symlink to this path right here in the current dest prefix.

        Normally we resolve symlinks. To retain symlinks, pass symlink=True.
        """
        if base is None:
            base = self.get_dst_prefix()

        # Since we use os.path.relpath() for this, which is purely textual, we
        # must ensure that both pathnames are absolute.
        if symlink:
            # symlink=True means: we know path is (or indirects through) a
            # symlink, don't resolve, we want to use the symlink.
            abspath = os.path.abspath
        else:
            # symlink=False means to resolve any symlinks we may find
            abspath = os.path.realpath

        return os.path.relpath(abspath(path), abspath(base))

    def set_github_output_path(self, variable, path):
        self.set_github_output(variable,
                               os.path.normpath(os.path.join(self.get_dst_prefix(), path)))

    def set_github_output(self, variable, *values):
        GITHUB_OUTPUT = os.getenv('GITHUB_OUTPUT')
        if GITHUB_OUTPUT and values:
            with open(GITHUB_OUTPUT, 'a') as outf:
                if len(values) == 1:
                    print('='.join((variable, values[0])), file=outf)
                else:
                    delim = secrets.token_hex(8)
                    print('<<'.join((variable, delim)), file=outf)
                    for value in values:
                        print(value, file=outf)
                    print(delim, file=outf)


class Windows_x86_64_Manifest(ViewerManifest):
    # We want the platform, per se, for every Windows build to be 'win'. The
    # VMP will concatenate that with the address_size.
    build_data_json_platform = 'win'
    address_size = 64

    def final_exe(self):
        return self.exec_name()+".exe"

    def finish_build_data_dict(self, build_data_dict):
        build_data_dict['Executable'] = self.final_exe()
        build_data_dict['AppName']    = self.app_name()
        return build_data_dict

    def test_msvcrt_and_copy_action(self, src, dst):
        # This is used to test a dll manifest.
        # It is used as a temporary override during the construct method
        from test_win32_manifest import test_assembly_binding
        # TODO: This is redundant with LLManifest.copy_action(). Why aren't we
        # calling copy_action() in conjunction with test_assembly_binding()?
        if src and (os.path.exists(src) or os.path.islink(src)):
            # ensure that destination path exists
            self.cmakedirs(os.path.dirname(dst))
            self.created_paths.append(dst)
            if not os.path.isdir(src):
                if(self.args['buildtype'].lower() == 'debug'):
                    test_assembly_binding(src, "Microsoft.VC80.DebugCRT", "8.0.50727.4053")
                else:
                    test_assembly_binding(src, "Microsoft.VC80.CRT", "8.0.50727.4053")
                self.ccopy(src,dst)
            else:
                raise Exception("Directories are not supported by test_CRT_and_copy_action()")
        else:
            print("Doesn't exist:", src)

    def test_for_no_msvcrt_manifest_and_copy_action(self, src, dst):
        # This is used to test that no manifest for the msvcrt exists.
        # It is used as a temporary override during the construct method
        from test_win32_manifest import test_assembly_binding
        from test_win32_manifest import NoManifestException, NoMatchingAssemblyException
        # TODO: This is redundant with LLManifest.copy_action(). Why aren't we
        # calling copy_action() in conjunction with test_assembly_binding()?
        if src and (os.path.exists(src) or os.path.islink(src)):
            # ensure that destination path exists
            self.cmakedirs(os.path.dirname(dst))
            self.created_paths.append(dst)
            if not os.path.isdir(src):
                try:
                    if(self.args['buildtype'].lower() == 'debug'):
                        test_assembly_binding(src, "Microsoft.VC80.DebugCRT", "")
                    else:
                        test_assembly_binding(src, "Microsoft.VC80.CRT", "")
                    raise Exception("Unknown condition")
                except NoManifestException as err:
                    pass
                except NoMatchingAssemblyException as err:
                    pass

                self.ccopy(src,dst)
            else:
                raise Exception("Directories are not supported by test_CRT_and_copy_action()")
        else:
            print("Doesn't exist:", src)

    def vlc_plugin_allowlist(self):
        """Plugin paths (relative to plugins/) from vlc_plugin_allowlist.txt."""
        listfile = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'vlc_plugin_allowlist.txt')
        with open(listfile, encoding='utf-8') as f:
            entries = [l.strip() for l in f if l.strip() and not l.lstrip().startswith('#')]
        if not entries:
            raise ManifestError("%s is empty" % listfile)
        dups = sorted(set(e for e in entries if entries.count(e) > 1))
        if dups:
            raise ManifestError("duplicate entries in %s: %s" % (listfile, dups))
        for e in entries:
            if e.startswith('/') or '..' in e.split('/') or '\\' in e or not e.endswith('.dll'):
                raise ManifestError("bad plugin allowlist entry %r" % e)
        return entries

    def construct(self):
        super().construct()

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        relpkgdir = os.path.join(pkgdir, "lib", "release")
        debpkgdir = os.path.join(pkgdir, "lib", "debug")

        if self.is_packaging_viewer():
            # Find slipstream-bin.exe in the 'configuration' dir, then rename it to the result of final_exe.
            self.path(src='%s/slipstream-bin.exe' % self.args['configuration'], dst=self.final_exe())
            # <GMDX> crash reports: Crashpad's handler beside the exe (when the build has it)
            if os.path.exists(os.path.join(self.args['build'], 'crashpad_handler.exe')):
                self.path('crashpad_handler.exe')

            # SDL window (USE_SDL_WINDOW): LLWindowSDL loads its custom cursors (sit,
            # grab, pay, media, build tools...) and the window icon from res-sdl/*.BMP.
            # Without them every custom cursor falls back to the arrow.
            self.path("res-sdl")
            with self.prefix(src=self.icon_path(), dst="res-sdl"):
                self.path("slipstream_256.BMP", "ll_icon.BMP")

            GITHUB_OUTPUT = os.getenv('GITHUB_OUTPUT')
            if GITHUB_OUTPUT:
                # Emit the whole app image as one of the GitHub step outputs. We
                # want the whole app -- but NOT the extraneous build products that
                # get tossed into the same directory, such as the installer and
                # the symbols tarball, so add exclusions. When we feed
                # upload-artifact multiple absolute pathnames, even just for
                # exclusion, it ends up creating several extraneous directory
                # levels within the artifact -- so try using only relative paths.
                # One problem: as of right now, our current directory os.getcwd()
                # is not the same as the initial working directory for this job
                # step, meaning paths relative to our os.getcwd() won't work for
                # the subsequent upload-artifact step. We're a couple directory
                # levels down. Try adjusting for those when specifying the base
                # for self.relpath().
                appbase = self.relpath(
                    self.get_dst_prefix(),
                    base=os.path.join(os.getcwd(), os.pardir, os.pardir),
                    symlink=True)
                self.set_github_output('viewer_app', appbase,
                                    # except for this stuff
                                    *(('!' + os.path.join(appbase, pattern))
                                        for pattern in (
                                                'slipstream-bin.*',
                                                '*_Setup.exe',
                                                '*.bat',
                                                '*.tar.xz')))

            # with self.prefix(src=os.path.join(pkgdir, "VMP")):
            #     # include the compiled launcher scripts so that it gets included in the file_list
            #     self.path('SLVersionChecker.exe')

            # with self.prefix(dst="vmp_icons"):
            #     with self.prefix(src=self.icon_path()):
            #         self.path("alchemy.ico")
            #     #VMP  Tkinter icons
            #     with self.prefix(src="vmp_icons"):
            #         self.path("*.png")
            #         self.path("*.gif")

        # Plugin host application
        self.path2basename(os.path.join(os.pardir,
                                        'llplugin', 'slplugin', self.args['configuration']),
                           "SLPlugin.exe")

        # Get shared libs from the shared libs staging directory
        with self.prefix(src=os.path.join(self.args['build'], os.pardir,
                                          'sharedlibs', self.args['buildtype'])):
            # WebRTC libraries
            for libfile in (
                    'llwebrtc.dll',
            ):
                self.path(libfile)

            if self.args['openal'] == 'ON':
                # Get openal dll
                self.path("openal32.dll")
                self.path("alut.dll")

            # For textures
            self.path("openjp2.dll")

            # #647: a staged DLL that differs from its 3p package copy is stale (the
            # staging step once kept OpenJPEG 2.4.0 beside an exe linked to 2.5.4).
            # Refuse to package it rather than ship an exe that won't start.
            pkg_dir = os.path.join(self.args['build'], os.pardir, 'packages', 'lib', 'release')
            if os.path.isdir(pkg_dir):
                stale = []
                for name in os.listdir(pkg_dir):
                    staged = os.path.join(self.get_src_prefix(), name)
                    if name.lower().endswith('.dll') and os.path.isfile(staged) \
                            and not filecmp.cmp(staged, os.path.join(pkg_dir, name), shallow=False):
                        stale.append(name)
                if stale:
                    raise ManifestError("staged DLLs differ from their 3p packages (stale copy): "
                                        + ", ".join(sorted(stale)))

            # These need to be installed as a SxS assembly, currently a 'private' assembly.
            # See http://msdn.microsoft.com/en-us/library/ms235291(VS.80).aspx
            self.path("msvcp140.dll")
            self.path_optional("msvcp140_1.dll")
            self.path_optional("msvcp140_2.dll")
            self.path_optional("msvcp140_atomic_wait.dll")
            self.path_optional("msvcp140_codecvt_ids.dll")
            self.path("vcruntime140.dll")
            self.path_optional("vcruntime140_1.dll")
            self.path_optional("vcruntime140_threads.dll")

            # SDL3
            self.path("SDL3.dll")

            if self.args['tracy'] == 'ON':
                with self.prefix(src=os.path.join(pkgdir, 'bin')):
                    self.path("tracy-profiler.exe")

        self.path(src="licenses-win32.txt", dst="licenses.txt")
        self.path("featuretable.txt")
        self.path("cube.dae")

        with self.prefix(src=pkgdir):
            self.path("ca-bundle.crt")

        # Media plugins - CEF
        with self.prefix(dst="llplugin"):
            with self.prefix(src=os.path.join(self.args['build'], os.pardir, 'media_plugins')):
                with self.prefix(src=os.path.join('cef', self.args['configuration'])):
                    self.path("media_plugin_cef.dll")

                # Media plugins - LibVLC
                with self.prefix(src=os.path.join('libvlc', self.args['configuration'])):
                    self.path("media_plugin_libvlc.dll")

                # Media plugins - Example (useful for debugging - not shipped with release viewer)
                if self.channel_type() != 'release':
                    with self.prefix(src=os.path.join('example', self.args['configuration'])):
                        self.path("media_plugin_example.dll")

            # CEF runtime files - debug
            # CEF runtime files - not debug (release, relwithdebinfo etc.)
            config = 'debug' if self.args['configuration'].lower() == 'debug' else 'release'
            with self.prefix(src=os.path.join(pkgdir, 'bin', config)):
                self.path("chrome_elf.dll")
                self.path("d3dcompiler_47.dll")
                self.path("dxcompiler.dll")
                self.path("dxil.dll")
                self.path("libcef.dll")
                self.path("libEGL.dll")
                self.path("libGLESv2.dll")
                self.path("v8_context_snapshot.bin")
                self.path("vk_swiftshader.dll")
                self.path("vk_swiftshader_icd.json")
                self.path("vulkan-1.dll")
                self.path("dullahan_host.exe")

            # MSVC DLLs needed for CEF and have to be in same directory as plugin
            with self.prefix(src=os.path.join(self.args['build'], os.pardir,
                                              'sharedlibs', self.args['buildtype'])):
                self.path("msvcp140.dll")
                self.path("vcruntime140.dll")
                self.path_optional("vcruntime140_1.dll")

            # CEF files common to all configurations
            with self.prefix(src=os.path.join(pkgdir, 'resources')):
                self.path("chrome_100_percent.pak")
                self.path("chrome_200_percent.pak")
                self.path("resources.pak")
                self.path("icudtl.dat")

            with self.prefix(src=os.path.join(pkgdir, 'resources', 'locales'), dst='locales'):
                self.path("am.pak")
                self.path("ar.pak")
                self.path("bg.pak")
                self.path("bn.pak")
                self.path("ca.pak")
                self.path("cs.pak")
                self.path("da.pak")
                self.path("de.pak")
                self.path("el.pak")
                self.path("en-GB.pak")
                self.path("en-US.pak")
                self.path("es-419.pak")
                self.path("es.pak")
                self.path("et.pak")
                self.path("fa.pak")
                self.path("fi.pak")
                self.path("fil.pak")
                self.path("fr.pak")
                self.path("gu.pak")
                self.path("he.pak")
                self.path("hi.pak")
                self.path("hr.pak")
                self.path("hu.pak")
                self.path("id.pak")
                self.path("it.pak")
                self.path("ja.pak")
                self.path("kn.pak")
                self.path("ko.pak")
                self.path("lt.pak")
                self.path("lv.pak")
                self.path("ml.pak")
                self.path("mr.pak")
                self.path("ms.pak")
                self.path("nb.pak")
                self.path("nl.pak")
                self.path("pl.pak")
                self.path("pt-BR.pak")
                self.path("pt-PT.pak")
                self.path("ro.pak")
                self.path("ru.pak")
                self.path("sk.pak")
                self.path("sl.pak")
                self.path("sr.pak")
                self.path("sv.pak")
                self.path("sw.pak")
                self.path("ta.pak")
                self.path("te.pak")
                self.path("th.pak")
                self.path("tr.pak")
                self.path("uk.pak")
                self.path("vi.pak")
                self.path("zh-CN.pak")
                self.path("zh-TW.pak")

            with self.prefix(src=os.path.join(pkgdir, 'bin', 'release')):
                self.path("libvlc.dll")
                self.path("libvlccore.dll")
                # Media plugin hardening: ship only the allowlisted libVLC modules
                # (see vlc_plugin_allowlist.txt). A listed module missing from the
                # 3p package raises MissingError and fails packaging. libVLC's
                # plugins.dat cache is not shipped; it is rebuilt/validated at runtime.
                with self.prefix(src='plugins', dst='plugins'):
                    for plugin in self.vlc_plugin_allowlist():
                        self.path(plugin)

        if not self.is_packaging_viewer():
            self.package_file = "copied_deps"

    def isc_file_commands(self):
        def INSTDIR(path):
            # Note that '$INSTDIR' is purely textual here: we write
            # exactly that into the .nsi file for NSIS to interpret.
            # Pass the result through normpath() to handle the case in which
            # path is the empty string. On Windows, that produces "$INSTDIR\".
            # Unfortunately, if that's the last item on a line, NSIS takes
            # that as line continuation and misinterprets the following line.
            # Ensure we don't emit a trailing backslash.
            return os.path.normpath(os.path.join('{app}', path))

        result = []
        dest_files = [pair[1] for pair in self.file_list if pair[0] and os.path.isfile(pair[1])]
        # sort deepest hierarchy first
        dest_files.sort(key=lambda f: (f.count(os.path.sep), f), reverse=True)
        out_path = None
        for pkg_file in dest_files:
            pkg_file = os.path.normpath(pkg_file)
            rel_file = self.relpath(pkg_file)
            installed_dir = INSTDIR(os.path.dirname(rel_file))
            result.append('Source: "{}"; DestDir: "{}"; Flags: ignoreversion'.format(rel_file, installed_dir))

        return '\n'.join(result)

    def package_finish(self):
        # a standard map of strings for replacing in the templates
        substitution_strings = {
            'version' : '.'.join(self.args['version']),
            'version_short' : '.'.join(self.args['version'][:-1]),
            'version_dashes' : '-'.join(self.args['version']),
            'version_registry' : '%s(64)' % '.'.join(self.args['version']),
            'final_exe' : self.final_exe(),
            'flags':'',
            'app_name':self.app_name(),
            'app_name_oneword':self.app_name_oneword()
            }

        installer_file = self.installer_base_name() + '_Setup.exe'
        substitution_strings['installer_file'] = self.installer_base_name() + '_Setup'

        if self.channel_type() == 'release':
            substitution_strings['caption'] = CHANNEL_VENDOR_BASE
        else:
            substitution_strings['caption'] = self.app_name() + ' ${VERSION}'

        inst_vars_template = """
#define MyAppName "%(app_name)s"
#define MyAppNameShort "%(app_name_oneword)s"
#define MyAppVersion "%(version)s"
#define MyAppShortVersion "%(version_short)s"
#define MyAppExeName "%(final_exe)s"
#define MyAppInstFile "%(installer_file)s"
            """

        # Dump the installers/windows directory into the raw app image tree
        # because NSIS needs those files. But don't use path() because we
        # don't want them installed with the viewer - they're only for use by
        # the installer itself.
        shutil.copytree(os.path.join(self.get_src_prefix(), 'installers', 'windows'),
                        os.path.join(self.get_dst_prefix(), 'installers', 'windows'),
                        dirs_exist_ok=True)

        tempfile = "slipstream_setup_tmp.iss"
        # the following replaces strings in the iss template
        # it also does python-style % substitution
        self.replace_in("installers/windows/installer_template.iss", tempfile, {
                "%%SOURCE%%":self.get_src_prefix(),
                "%%INST_VARS%%":inst_vars_template % substitution_strings,
                "%%INSTALL_FILES%%":self.isc_file_commands()})

        self.package_file = installer_file


class Darwin_x86_64_Manifest(ViewerManifest):
    build_data_json_platform = 'mac'
    address_size = 64

    def finish_build_data_dict(self, build_data_dict):
        build_data_dict.update({'Bundle Id':self.args['bundleid']})
        return build_data_dict

    def is_packaging_viewer(self):
        # darwin requires full app bundle packaging even for debugging.
        return True

    def is_rearranging(self):
        # That said, some stuff should still only be performed once.
        # Are either of these actions in 'actions'? Is the set intersection
        # non-empty?
        return bool(set(["package", "unpacked"]).intersection(self.args['actions']))

    def construct(self):
        # copy over the build result (this is a no-op if run within the xcode
        # script)
        self.path(os.path.join(self.args['configuration'], self.channel() + ".app"), dst="")

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        relpkgdir = os.path.join(pkgdir, "lib", "release")
        debpkgdir = os.path.join(pkgdir, "lib", "debug")

        with self.prefix(src="", dst="Contents"):  # everything goes in Contents
            # CEF framework goes inside Contents/Frameworks.
            # Remember where we parked this car.
            with self.prefix(src=relpkgdir, dst="Frameworks"):
                self.path("libndofdev.dylib")
                self.path("libSDL3.dylib")


                # OpenAL dylibs
                if self.args['openal'] == 'ON':
                    for libfile in (
                                "libopenal.dylib",
                                "libalut.dylib",
                                ):
                        self.path(libfile)

                # WebRTC libraries
                with self.prefix(src=os.path.join(self.args['build'], os.pardir,
                                          'sharedlibs', self.args['buildtype'], 'Resources')):
                    for libfile in (
                            'libllwebrtc.dylib',
                    ):
                        self.path(libfile)

            with self.prefix(dst="MacOS"):
                executable = self.dst_path_of(self.channel())
                # NOTE: the -S argument to strip causes it to keep
                # enough info for annotated backtraces (i.e. function
                # names in the crash log). 'strip' with no arguments
                # yields a slightly smaller binary but makes crash
                # logs mostly useless. This may be desirable for the
                # final release. Or not.
                if ("package" in self.args['actions'] or
                    "unpacked" in self.args['actions']):
                    self.run_command(
                        ['strip', '-S', executable])

            with self.prefix(dst="Resources"):
                # defer cross-platform file copies until we're in the
                # nested Resources directory
                super().construct()

                # need .icns file referenced by Info.plist
                with self.prefix(src=self.icon_path(), dst="") :
                    self.path("slipstream.icns")

                # # Copy in the updater script and helper modules
                # self.path(src=os.path.join(pkgdir, 'VMP'), dst="updater")

                # with self.prefix(src="", dst=os.path.join("updater", "icons")):
                #     self.path2basename(self.icon_path(), "secondlife.ico")
                #     with self.prefix(src="vmp_icons", dst=""):
                #         self.path("*.png")
                #         self.path("*.gif")

                with self.prefix(src_dst="cursors_mac"):
                    self.path("*.tif")

                self.path("licenses-mac.txt", dst="licenses.txt")
                self.path("featuretable_mac.txt")
                self.path("cube.dae")

                with self.prefix(src=pkgdir,dst=""):
                    self.path("ca-bundle.crt")

                # Translations
                self.path("English.lproj/language.txt")
                self.replace_in(src="English.lproj/InfoPlist.strings",
                                dst="English.lproj/InfoPlist.strings",
                                searchdict={'%%VERSION%%':'.'.join(self.args['version'])}
                                )
                self.path("German.lproj")
                self.path("Japanese.lproj")
                self.path("Korean.lproj")
                self.path("da.lproj")
                self.path("es.lproj")
                self.path("fr.lproj")
                self.path("hu.lproj")
                self.path("it.lproj")
                self.path("nl.lproj")
                self.path("pl.lproj")
                self.path("pt.lproj")
                self.path("ru.lproj")
                self.path("tr.lproj")
                self.path("uk.lproj")
                self.path("zh-Hans.lproj")

                def path_optional(src, dst):
                    """
                    For a number of our self.path() calls, not only do we want
                    to deal with the absence of src, we also want to remember
                    which were present. Return either an empty list (absent)
                    or a list containing dst (present). Concatenate these
                    return values to get a list of all libs that are present.
                    """
                    # This was simple before we started needing to pass
                    # wildcards. Fortunately, self.path() ends up appending a
                    # (source, dest) pair to self.file_list for every expanded
                    # file processed. Remember its size before the call.
                    oldlen = len(self.file_list)
                    try:
                        self.path(src, dst)
                        # The dest appended to self.file_list has been prepended
                        # with self.get_dst_prefix(). Strip it off again.
                        added = [os.path.relpath(d, self.get_dst_prefix())
                                 for s, d in self.file_list[oldlen:]]
                    except MissingError as err:
                        print("Warning: "+err.msg, file=sys.stderr)
                        added = []
                    if not added:
                        print("Skipping %s" % dst)
                    return added

                # dylibs is a list of all the .dylib files we expect to need
                # in our bundled sub-apps. For each of these we'll create a
                # symlink from sub-app/Contents/Resources to the real .dylib.
                # Need to get the llcommon dll from any of the build directories as well.
                libfile_parent = self.get_dst_prefix()
                dylibs=[]
                # our apps
                executable_path = {}
                embedded_apps = [ (os.path.join("llplugin", "slplugin"), "SLPlugin.app") ]
                for app_bld_dir, app in embedded_apps:
                    self.path2basename(os.path.join(os.pardir,
                                                    app_bld_dir, self.args['configuration']),
                                       app)
                    executable_path[app] = \
                        self.dst_path_of(os.path.join(app, "Contents", "MacOS"))

                # Dullahan helper apps go inside SLPlugin.app
                with self.prefix(dst=os.path.join(
                    "SLPlugin.app", "Contents", "Frameworks")):
                    # copy CEF plugin
                    self.path2basename("../media_plugins/cef/" + self.args['configuration'],
                                       "media_plugin_cef.dylib")

                    # copy LibVLC plugin
                    self.path2basename("../media_plugins/libvlc/" + self.args['configuration'],
                                       "media_plugin_libvlc.dylib")

                    # CEF framework and vlc libraries goes inside Contents/Frameworks.
                    with self.prefix(src=os.path.join(pkgdir, 'lib', 'release')):
                        self.path("Chromium Embedded Framework.framework")
                        self.path("DullahanHelper.app")
                        self.path("DullahanHelper (Alerts).app")
                        self.path("DullahanHelper (GPU).app")
                        self.path("DullahanHelper (Renderer).app")
                        self.path("DullahanHelper (Plugin).app")

                        # Copy libvlc
                        self.path( "libvlc*.dylib*" )
                        # copy LibVLC plugins folder
                        with self.prefix(src='plugins', dst="plugins"):
                            self.path( "*.dylib" )
                            self.path( "plugins.dat" )


    def package_finish(self):
        imagename = self.installer_base_name()
        self.set_github_output('imagename', imagename)
        finalname = imagename + ".dmg"
        self.package_file = finalname

        RUNNER_TEMP = os.getenv('RUNNER_TEMP')
        # When running as a GitHub Action job, RUNNER_TEMP is the recommended
        # temp directory. If we're not running on GitHub, don't create this
        # temp directory or this tarball: we don't clean them up, trusting
        # that the runner is itself transient. On a dev machine, that would
        # result in temp-directory clutter.
        if RUNNER_TEMP:
            # Per GitHub's actions/upload-artifact documentation
            # https://github.com/actions/upload-artifact#maintaining-file-permissions-and-case-sensitive-files
            # we must package the app bundle with tar before posting as an
            # artifact. Posting individual files follows symlinks, which
            # causes problems, especially with frameworks: a framework's top
            # level must contain symlinks into its Versions/Current, which
            # must itself be a symlink to some specific Versions subdir.
            tarpath = os.path.join(RUNNER_TEMP, "viewer.tar.xz")
            print(f'Creating {tarpath} from {self.get_dst_prefix()}')
            with tarfile.open(tarpath, mode="w:xz") as tarball:
                # Store in the tarball as just 'Second Life Mumble.app'
                # instead of 'Users/someone/.../newview/Release/Second...'
                # It's at this point that we rename 'Second Life Release.app'
                # to 'Second Life Viewer.app'.
                tarball.add(self.get_dst_prefix(),
                            arcname=self.app_name() + ".app")
            self.set_github_output_path('viewer_app', tarpath)


class LinuxManifest(ViewerManifest):
    build_data_json_platform = 'lnx'

    def construct(self):
        super(LinuxManifest, self).construct()

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        if "package_dir" in self.args:
            pkgdir = self.args['package_dir']

        relpkgdir = os.path.join(pkgdir, "lib", "release")
        debpkgdir = os.path.join(pkgdir, "lib", "debug")

        self.path("licenses-linux.txt","licenses.txt")
        with self.prefix("linux_tools"):
            self.path("client-readme.txt","README-linux.txt")
            self.path("client-readme-voice.txt","README-linux-voice.txt")
            self.path("client-readme-joystick.txt","README-linux-joystick.txt")
            self.path("wrapper.sh","gunmetal-dx")   # <GMDX>
            with self.prefix(dst="etc"):
                self.path("handle_secondlifeprotocol.sh")
                self.path("register_secondlifeprotocol.sh")
                self.path("refresh_desktop_app_entry.sh")
                self.path("launch_url.sh")
                self.path("update_hooks.sh")
            self.path("install.sh")

        with self.prefix(dst="bin"):
            self.path("slipstream-bin", "do-not-directly-run-slipstream-bin")
            self.path2basename("../llplugin/slplugin", "SLPlugin")
            # <GMDX> crash reports: Crashpad's handler beside the binary (when the build has it)
            if os.path.exists(os.path.join(self.args['build'], 'crashpad_handler')):
                self.path('crashpad_handler')
            #this copies over the python wrapper script, associated utilities and required libraries, see SL-321, SL-322 and SL-323
            #with self.prefix(src="../viewer_components/manager", dst=""):
            #    self.path("*.py")

        # recurses, packaged again
        self.path("res-sdl")

        # We  copy ll_icon.BMP in CMakeLists.txt to newview/res-sdl and this will let the above self.path step take  care of copying
        # the correct branded icon
        # Get the icons based on the channel type
        icon_path = self.icon_path()
        with self.prefix(src=icon_path) :
            self.path("slipstream_256.png","slipstream_icon.png")
            with self.prefix(dst="res-sdl") :
                self.path("slipstream_256.BMP","ll_icon.BMP")

        # hicolor icon theme set, for distribution packages to install under
        # /usr/share/icons (scripts/packaging/aur/PKGBUILD, #248).
        for size in ("16x16", "24x24", "32x32", "48x48", "64x64", "128x128", "256x256", "512x512", "scalable"):
            subdir = os.path.join("hicolor", size, "apps")
            with self.prefix(src=os.path.join(icon_path, "linux", subdir), dst=os.path.join("share", "icons", subdir)):
                self.path("slipstream.svg" if size == "scalable" else "slipstream.png")

        with self.prefix(src=os.path.join(self.args['build'], os.pardir, "llwebrtc" ), dst="lib"):
            self.path("libllwebrtc.so")

        # plugins
        with self.prefix(dst="bin/llplugin"):
            with self.prefix(src=os.path.join(self.args['build'], os.pardir, 'media_plugins')):
                with self.prefix(src='cef'):
                    self.path("libmedia_plugin_cef.so")

                # Media plugins - LibVLC
                with self.prefix(src='libvlc'):
                    self.path("libmedia_plugin_libvlc.so")

                # GStreamer 1.0 Media Plugin
                with self.prefix(src='gstreamer10'):
                    self.path("libmedia_plugin_gstreamer10.so")

                # Media plugins - Example (useful for debugging - not shipped with release viewer)
                if self.channel_type() != 'release':
                    with self.prefix(src='example'):
                        self.path("libmedia_plugin_example.so")


        with self.prefix(src=os.path.join(pkgdir, 'lib', 'release'), dst="lib"):
            self.path( "libcef.so" )
            self.path( "libEGL*" )
            self.path( "libvulkan*" )
            self.path( "libvk_swiftshader*" )
            self.path( "libGLESv2*" )


        with self.prefix(src=os.path.join(pkgdir, 'bin', 'release'), dst="bin"):
            self.path( "chrome-sandbox" )
            self.path( "dullahan_host" )

        # Since dullahan 1.50 / CEF 152 the package ships these next to libcef.so
        # (lib/release) instead of bin/release (#390). Still installed into both bin/
        # and lib/, as before.
        with self.prefix(src=os.path.join(pkgdir, 'lib', 'release'), dst="bin"):
            self.path( "v8_context_snapshot.bin" )
            self.path( "vk_swiftshader_icd.json")

        with self.prefix(src=os.path.join(pkgdir, 'lib', 'release'), dst="lib"):
            self.path( "v8_context_snapshot.bin" )
            self.path( "vk_swiftshader_icd.json")

        with self.prefix(src=os.path.join(pkgdir, 'resources'), dst="lib"):
            self.path( "chrome_100_percent.pak" )
            self.path( "chrome_200_percent.pak" )
            self.path( "resources.pak" )
            self.path( "icudtl.dat" )

        with self.prefix(src=os.path.join(pkgdir, 'resources', 'locales'), dst=os.path.join('lib', 'locales')):
            self.path("*.pak")

        self.path("featuretable_linux.txt")
        self.path("cube.dae")

        with self.prefix(src=pkgdir, dst="bin"):
            self.path("ca-bundle.crt")

    def package_finish(self):
        installer_name = self.installer_base_name()

        # When running as a GitHub Action job, RUNNER_TEMP is defined as the tmp dir
        RUNNER_TEMP = os.getenv('RUNNER_TEMP')

        self.strip_binaries()

        # dullahan_host (CEF's helper process, prebuilt by the dullahan 3p
        # package) ships with RUNPATH ":/../lib:/home/runner/work/...": an empty
        # element (= current directory), the system /lib, and the 3p builder's
        # path, so it finds lib/libcef.so only through the launcher's
        # LD_LIBRARY_PATH. Point it at our lib/ like the CEF plugin's
        # $ORIGIN/../../lib. #245
        self.set_elf_runpath(os.path.join(self.get_dst_prefix(), 'bin', 'dullahan_host'),
                             '$ORIGIN/../lib')
        # The CEF media plugin's RUNPATH comes out of the Ninja build with the
        # shell quotes baked in ("'$ORIGIN:$ORIGIN/../../lib'", from upstream's
        # -rpath,'...' link option), so neither element is valid and the plugin
        # finds lib/libcef.so only through the launcher's LD_LIBRARY_PATH: start
        # the binary any other way (desktop entry, file manager) and the plugin
        # fails to load with no reason in the log. Strip the quotes. #245
        self.set_elf_runpath(os.path.join(self.get_dst_prefix(), 'bin', 'llplugin', 'libmedia_plugin_cef.so'),
                             '$ORIGIN:$ORIGIN/../../lib')

        # Fix access permissions
        self.run_command(['find', self.get_dst_prefix(),
                          '-type', 'd', '-exec', 'chmod', '755', '{}', ';'])
        for old, new in ('0700', '0755'), ('0500', '0555'), ('0600', '0644'), ('0400', '0444'):
            self.run_command(['find', self.get_dst_prefix(),
                              '-type', 'f', '-perm', old,
                              '-exec', 'chmod', new, '{}', ';'])
        self.package_file = installer_name + '.tar.xz'

        # temporarily move directory tree so that it has the right
        # name in the tarfile
        realname = self.get_dst_prefix()
        versionedName = self.build_path_of(installer_name)

        tarName = versionedName + ".tar.xz"

        # If using a github runner we divert packaging a little. Considering this wil be a VM/docker image
        # we can just pack the final installer into RUNNER_TEMP and not into the usual stop we'd pick when
        # not building a GHA release
        if RUNNER_TEMP:
            tarName = os.path.join(RUNNER_TEMP, self.package_file)

        self.run_command(["mv", realname, versionedName])

        try:
            # only create tarball if it's a release build. Local dev builds
            # (Makefile) set ALCH_SKIP_TARBALL=1: the xz of the ~330MB tree
            # costs minutes per build and the viewer runs from packaged/.
            if self.args['buildtype'].lower() == 'release' and not os.environ.get('ALCH_SKIP_TARBALL'):
                # --numeric-owner hides the username of the builder for
                # security etc.
                self.run_command(['tar', '-C', self.get_build_prefix(),
                                  '--numeric-owner', '-cJf',
                                 tarName, installer_name])
                self.set_github_output_path('viewer_app', tarName)
            else:
                print("Skipping %s.tar.xz (buildtype=%s, ALCH_SKIP_TARBALL=%s)" % \
                      (installer_name, self.args['buildtype'],
                       os.environ.get('ALCH_SKIP_TARBALL', '')))
        finally:
            self.run_command(["mv", versionedName, realname])

    @staticmethod
    def set_elf_runpath(path, runpath):
        """Rewrite an ELF64 DT_RUNPATH/DT_RPATH string in place (no patchelf on
        the builders). Only shortens: the new string plus its NUL must fit in
        the old one's bytes; the remainder is NUL-filled."""
        import struct
        if not os.path.isfile(path):
            print("set_elf_runpath: %s not found, skipping" % path)
            return
        with open(path, 'r+b') as f:
            data = bytearray(f.read())
            if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
                print("set_elf_runpath: %s is not a little-endian ELF64, skipping" % path)
                return
            shoff, = struct.unpack_from('<Q', data, 0x28)
            shentsize, shnum = struct.unpack_from('<HH', data, 0x3A)
            sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize)
                        for i in range(shnum)]
            new = runpath.encode() + b'\0'
            for (_name, sh_type, _flags, _addr, sh_offset, sh_size, sh_link,
                 _info, _align, _entsize) in sections:
                if sh_type != 6:  # SHT_DYNAMIC
                    continue
                strtab_off = sections[sh_link][4]
                for pos in range(sh_offset, sh_offset + sh_size, 16):
                    tag, val = struct.unpack_from('<qQ', data, pos)
                    if tag == 0:  # DT_NULL
                        break
                    if tag not in (15, 29):  # DT_RPATH, DT_RUNPATH
                        continue
                    start = strtab_off + val
                    end = data.index(b'\0', start)
                    old = bytes(data[start:end])
                    if old == runpath.encode():
                        return
                    if len(new) > end - start + 1:
                        print("set_elf_runpath: %s: '%s' does not fit over '%s', skipping"
                              % (path, runpath, old.decode(errors='replace')))
                        return
                    # The linker may tail-merge strings: refuse if any symbol
                    # name or other dynamic string points into this one.
                    refs = []
                    for p2 in range(sh_offset, sh_offset + sh_size, 16):
                        t2, v2 = struct.unpack_from('<qQ', data, p2)
                        if t2 == 0:
                            break
                        if t2 in (1, 14):  # DT_NEEDED, DT_SONAME
                            refs.append(v2)
                    for s in sections:
                        if s[1] == 11 and s[6] == sh_link:  # SHT_DYNSYM using this strtab
                            for sp in range(s[4], s[4] + s[5], 24):
                                refs.append(struct.unpack_from('<I', data, sp)[0])
                    if any(val < r <= val + (end - start) for r in refs):
                        print("set_elf_runpath: %s: RUNPATH string is shared, skipping" % path)
                        return
                    data[start:end + 1] = new + b'\0' * (end + 1 - start - len(new))
                    # Write a new file and rename it over the old one: rewriting
                    # in place would SIGBUS a plugin host already running from it.
                    tmp = path + '.runpath-tmp'
                    with open(tmp, 'wb') as out:
                        out.write(data)
                    shutil.copymode(path, tmp)
                    os.replace(tmp, path)
                    print("set_elf_runpath: %s: '%s' -> '%s'" % (path, old.decode(errors='replace'), runpath))
                    return
            print("set_elf_runpath: %s has no RUNPATH, skipping" % path)

    def archive_debug_symbols(self):
        """Keep the viewer executable's debug info for the build we are packaging.

        The packaged binary is stripped (-S) and the build-dir binary is replaced
        by the next link, so a crash in a binary launched earlier could no longer
        be symbolized. Archive a build-id-keyed, zstd-compressed debug file
        (objcopy --only-keep-debug; ~1.3 GB vs 3.5 GB raw, ~12 s) to
            build-linux-x86_64/symbols/.build-id/<id[0:2]>/<id[2:]>.debug
        which is the layout gdb's debug-file-directory expects. The fatal-signal
        context on stderr prints the same build-id and path (llapp.cpp). Use:
            addr2line -Cfip -e build-linux-x86_64/symbols/.build-id/ab/cdef....debug 0xOFFSET
            gdb -iex 'set debug-file-directory build-linux-x86_64/symbols' <binary> <core>
        Bounded: the newest SLIPSTREAM_KEEP_SYMBOLS (default 3) are kept, by
        mtime; re-packaging an unchanged binary only refreshes its mtime.
        SLIPSTREAM_KEEP_SYMBOLS=0 disables archiving.
        """
        try:
            keep = int(os.environ.get('SLIPSTREAM_KEEP_SYMBOLS', '3'))
        except ValueError:
            keep = 3
        if keep <= 0:
            return
        exe = os.path.join(self.args['build'], 'slipstream-bin')
        if not os.path.isfile(exe):
            print("archive_debug_symbols: %s not found, skipping" % exe)
            return
        try:
            notes = subprocess.check_output(['readelf', '-n', exe], text=True,
                                            stderr=subprocess.DEVNULL)
        except (OSError, subprocess.CalledProcessError) as err:
            print("archive_debug_symbols: readelf failed (%s), skipping" % err)
            return
        m = re.search(r'Build ID:\s*([0-9a-fA-F]+)', notes)
        if not m:
            print("archive_debug_symbols: %s has no GNU build-id, skipping" % exe)
            return
        build_id = m.group(1).lower()
        symroot = os.path.normpath(os.path.join(self.args['build'], os.pardir, 'symbols'))
        idroot = os.path.join(symroot, '.build-id')
        dest_dir = os.path.join(idroot, build_id[:2])
        dest = os.path.join(dest_dir, build_id[2:] + '.debug')
        if os.path.exists(dest):
            os.utime(dest)  # most recently packaged => last to be pruned
        else:
            os.makedirs(dest_dir, exist_ok=True)
            tmp = dest + '.tmp'
            ok = False
            # zstd needs binutils >= 2.40; fall back to zlib, then uncompressed.
            for compress in (['--compress-debug-sections=zstd'],
                             ['--compress-debug-sections=zlib'], []):
                if subprocess.call(['objcopy', '--only-keep-debug'] + compress + [exe, tmp],
                                   stderr=subprocess.DEVNULL) == 0:
                    ok = True
                    break
            if not ok:
                if os.path.exists(tmp):
                    os.unlink(tmp)
                print("archive_debug_symbols: objcopy failed for %s" % exe)
                return
            os.replace(tmp, dest)
        print("archive_debug_symbols: build-id %s -> %s" % (build_id, dest))

        # Prune to the newest `keep` (plus any stale .tmp from an interrupted run).
        archived = []
        for path in glob.glob(os.path.join(idroot, '*', '*.debug*')):
            if path.endswith('.tmp'):
                os.unlink(path)
                continue
            archived.append(path)
        archived.sort(key=os.path.getmtime, reverse=True)
        for path in archived[keep:]:
            print("archive_debug_symbols: pruning %s" % path)
            os.unlink(path)
        for d in glob.glob(os.path.join(idroot, '*')):
            if os.path.isdir(d) and not os.listdir(d):
                os.rmdir(d)

    def strip_binaries(self):
        if self.is_packaging_viewer():
            self.archive_debug_symbols()
        if self.args['buildtype'].lower() == 'release' and self.is_packaging_viewer():
            print("* Going strip-crazy on the packaged binaries, since this is a Release build")
            # makes some small assumptions about our packaged dir structure
            # Strip into a temp file and rename it over the original. binutils'
            # strip rewrites the output file in place, which SIGBUSes a viewer
            # already running from this tree (its mapped .so pages change under it).
            skip = ('.py', '.pak', '.bin', '.dat', '.crt', '.dll', '.lib', '.json')
            for dir in ('bin', 'lib'):
                for root, _, files in os.walk(os.path.join(self.get_dst_prefix(), dir)):
                    for name in files:
                        path = os.path.join(root, name)
                        if name.endswith(skip) or name == 'update_install' or os.path.islink(path):
                            continue
                        tmp = path + '.strip-tmp'
                        if subprocess.call(['strip', '-S', '-o', tmp, path],
                                           stderr=subprocess.DEVNULL) == 0:
                            shutil.copymode(path, tmp)
                            os.replace(tmp, path)
                        elif os.path.exists(tmp):
                            os.unlink(tmp)

class Linux_x86_64_Manifest(LinuxManifest):
    address_size = 64

    def construct(self):
        super(Linux_x86_64_Manifest, self).construct()

        pkgdir = os.path.join(self.args['build'], os.pardir, 'packages')
        if "package_dir" in self.args:
            pkgdir = self.args['package_dir']

        relpkgdir = os.path.join(pkgdir, "lib", "release")
        #debpkgdir = os.path.join(pkgdir, "lib", "debug")

        with self.prefix(src=relpkgdir, dst="lib"):
            self.path("libSDL*.so.*")

            self.path("libalut.so*")
            self.path("libopenal.so*")

        self.copy_libdecor()

        self.strip_binaries()

    # Where distributions put libdecor: Debian/Ubuntu multiarch, Fedora/openSUSE,
    # Arch. Each has <libdir>/libdecor-0.so.0 and <libdir>/libdecor/plugins-1/.
    LIBDECOR_HOST_LIBDIRS = ('/usr/lib/x86_64-linux-gnu', '/usr/lib64', '/usr/lib')

    def copy_libdecor(self):
        """libdecor + its cairo plugin, from the build host (#196/#197).

        On a Wayland compositor without server-side decorations (GNOME/Mutter)
        SDL3 draws the window frame by dlopen'ing libdecor-0.so.0, which loads a
        plugin; without them the window has no title bar or border. Not every
        distribution installs libdecor, so the package carries one in
        lib/libdecor/ -- deliberately NOT lib/, which is on LD_LIBRARY_PATH and
        would shadow a system copy. The viewer (llsdl.cpp) prefers the system's
        libdecor when it has plugins (GTK plugin, desktop theme) and otherwise
        preloads this one by path and points LIBDECOR_PLUGIN_DIR at
        lib/libdecor/plugins-1. The cairo plugin only needs cairo, pango and
        dbus from the system, which every desktop has.

        No RUNPATH fix needed: the plugin's DT_NEEDED libdecor-0.so.0 resolves
        to the already-loaded copy by SONAME.

        SLIPSTREAM_LIBDECOR_LIBDIR overrides the search (a directory laid out
        like the ones above). Use it for a tarball built on a distribution whose
        packages target x86-64-v3/v4 (CachyOS; `make glibc-check` warns): e.g.
        extract Arch's generic extra/libdecor package and point it at <dir>/usr/lib.
        Release builds (CI, Ubuntu 24.04) copy generic x86-64 files needing
        GLIBC_2.38 at most."""
        override = os.environ.get('SLIPSTREAM_LIBDECOR_LIBDIR')
        candidates = (override,) if override else self.LIBDECOR_HOST_LIBDIRS
        for libdir in candidates:
            lib = os.path.join(libdir, 'libdecor-0.so.0')
            plugin = os.path.join(libdir, 'libdecor', 'plugins-1', 'libdecor-cairo.so')
            if os.path.isfile(lib) and os.path.isfile(plugin):
                break
        else:
            raise ManifestError(
                "libdecor not found on the build host (looked for libdecor-0.so.0 and "
                "libdecor/plugins-1/libdecor-cairo.so in %s). The Linux package ships them so "
                "the window has a title bar on GNOME Wayland (#196/#197). Install them:\n"
                "  Debian/Ubuntu: sudo apt install libdecor-0-0 libdecor-0-plugin-1-cairo\n"
                "  Fedora:        sudo dnf install libdecor\n"
                "  Arch:          sudo pacman -S libdecor\n"
                "or set SLIPSTREAM_LIBDECOR_LIBDIR to a directory holding both."
                % ', '.join(candidates))
        # libdecor-0.so.0 is a symlink to the versioned file: copy the file
        # itself under the SONAME (the manifest copies symlinks as symlinks).
        self.path(os.path.realpath(lib), os.path.join('lib', 'libdecor', 'libdecor-0.so.0'))
        self.path(os.path.realpath(plugin), os.path.join('lib', 'libdecor', 'plugins-1', 'libdecor-cairo.so'))

################################################################

if __name__ == "__main__":
    # Report our own command line so that, in case of trouble, a developer can
    # manually rerun the same command.
    print(('%s \\\n%s' %
          (sys.executable,
           ' '.join((("'%s'" % arg) if ' ' in arg else arg) for arg in sys.argv))))
    extra_arguments = [
        dict(name='openal', description="""Indication openal libraries are needed""", default='OFF'),
        dict(name='tracy', description="""Indication tracy profiler is enabled""", default='OFF'),
        ]
    try:
        main(extra=extra_arguments)
    except (ManifestError, MissingError) as err:
        sys.exit("\nviewer_manifest.py failed: "+err.msg)
    except:
        raise
