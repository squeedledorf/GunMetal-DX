/**
 * @file llcontrol_tut.cpp
 * @date   February 2008
 * @brief control group unit tests
 *
 * $LicenseInfo:firstyear=2008&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "llsdserialize.h"
#include "llfile.h"
#include "stringize.h"

#include "../llcontrol.h"
#include "v3math.h"

#include "../test/lltut.h"
#include <memory>
#include <vector>

namespace tut
{
    struct control_group
    {
        std::unique_ptr<LLControlGroup> mCG;
        std::string mTestConfigDir;
        std::string mTestConfigFile;
        std::vector<std::string> mCleanups;
        static bool mListenerFired;
        control_group()
        {
            mCG.reset(new LLControlGroup("foo"));
            LLUUID random;
            random.generate();
            // generate temp dir
            mTestConfigDir = STRINGIZE(LLFile::tmpdir() << "llcontrol-test-" << random << "/");
            mTestConfigFile = mTestConfigDir + "settings.xml";
            LLFile::mkdir(mTestConfigDir);
            LLSD config;
            config["TestSetting"]["Comment"] = "Dummy setting used for testing";
            config["TestSetting"]["Persist"] = 1;
            config["TestSetting"]["Type"] = "U32";
            config["TestSetting"]["Value"] = 12;
            writeSettingsFile(config);
        }
        ~control_group()
        {
            //Remove test files
            for (auto filename : mCleanups)
            {
                LLFile::remove(filename);
            }
            LLFile::remove(mTestConfigFile);
            LLFile::rmdir(mTestConfigDir);
        }
        void writeSettingsFile(const LLSD& config)
        {
            llofstream file(mTestConfigFile.c_str());
            if (file.is_open())
            {
                LLSDSerialize::toPrettyXML(config, file);
            }
            file.close();
        }
        static bool handleListenerTest()
        {
            control_group::mListenerFired = true;
            return true;
        }
    };

    bool control_group::mListenerFired = false;

    typedef test_group<control_group> control_group_test;
    typedef control_group_test::object control_group_t;
    control_group_test tut_control_group("control_group");

    //load settings from files - LLSD
    template<> template<>
    void control_group_t::test<1>()
    {
        int results = mCG->loadFromFile(mTestConfigFile.c_str());
        ensure("number of settings", (results == 1));
        ensure("value of setting", (mCG->getU32("TestSetting") == 12));
    }

    //save settings to files
    template<> template<>
    void control_group_t::test<2>()
    {
        int results = mCG->loadFromFile(mTestConfigFile.c_str());
        mCG->setU32("TestSetting", 13);
        ensure_equals("value of changed setting", mCG->getU32("TestSetting"), 13);
        LLControlGroup test_cg("foo2");
        std::string temp_test_file = (mTestConfigDir + "setting_llsd_temp.xml");
        mCleanups.push_back(temp_test_file);
        mCG->saveToFile(temp_test_file.c_str(), true);
        results = test_cg.loadFromFile(temp_test_file.c_str());
        ensure("number of changed settings loaded", (results == 1));
        ensure("value of changed settings loaded", (test_cg.getU32("TestSetting") == 13));
    }

    //priorities
    template<> template<>
    void control_group_t::test<3>()
    {
        // Pass default_values = true. This tells loadFromFile() we're loading
        // a default settings file that declares variables, rather than a user
        // settings file. When loadFromFile() encounters an unrecognized user
        // settings variable, it forcibly preserves it (CHOP-962).
        int results = mCG->loadFromFile(mTestConfigFile.c_str(), true);
        LLControlVariable* control = mCG->getControl("TestSetting");
        LLSD new_value = 13;
        control->setValue(new_value, false);
        ensure_equals("value of changed setting", mCG->getU32("TestSetting"), 13);
        LLControlGroup test_cg("foo3");
        std::string temp_test_file = (mTestConfigDir + "setting_llsd_persist_temp.xml");
        mCleanups.push_back(temp_test_file);
        mCG->saveToFile(temp_test_file.c_str(), true);
        results = test_cg.loadFromFile(temp_test_file.c_str());
        //If we haven't changed any settings, then we shouldn't have any settings to load
        ensure("number of non-persisted changed settings loaded", (results == 0));
    }

    //listeners
    template<> template<>
    void control_group_t::test<4>()
    {
        int results = mCG->loadFromFile(mTestConfigFile.c_str());
        ensure("number of settings", (results == 1));
        mCG->getControl("TestSetting")->getSignal()->connect(boost::bind(&this->handleListenerTest));
        mCG->setU32("TestSetting", 13);
        ensure("listener fired on changed setting", mListenerFired);
    }

    // #126: values saved under a renamed setting's old name carry over to the new name
    template<> template<>
    void control_group_t::test<5>()
    {
        mCG->declareU32("NewU32", 1, "renamed U32");
        mCG->declareString("NewString", "", "renamed string");
        mCG->declareBOOL("NewKeptBool", false, "the file also has the new name");
        mCG->declareS32("NewAlreadySet", 0, "set before the migration");
        mCG->declareU32("NewTypeChanged", 0, "type differs from the old one");
        mCG->declareVec3("NewVec3", LLVector3(0.f, 0.f, 0.f), "renamed vector");
        mCG->declareControl("NewNotPersisted", TYPE_BOOLEAN, false, "not persisted", LLControlVariable::PERSIST_NO);
        mCG->setS32("NewAlreadySet", 5);

        // What a user settings file saved by the previous release looks like.
        LLSD file;
        file["OldU32"]["Type"] = "U32";
        file["OldU32"]["Value"] = 7;
        file["OldString"]["Type"] = "String";
        file["OldString"]["Value"] = "hello";
        file["OldKeptBool"]["Type"] = "Boolean";
        file["OldKeptBool"]["Value"] = true;
        file["NewKeptBool"]["Type"] = "Boolean";
        file["NewKeptBool"]["Value"] = false;
        file["OldAlreadySet"]["Type"] = "S32";
        file["OldAlreadySet"]["Value"] = 9;
        file["OldTypeChanged"]["Type"] = "F32";
        file["OldTypeChanged"]["Value"] = 2.5;
        file["OldVec3"]["Type"] = "Vector3";
        file["OldVec3"]["Value"] = LLVector3(1.f, 2.f, 3.f).getValue();
        file["OldNotPersisted"]["Type"] = "Boolean";
        file["OldNotPersisted"]["Value"] = true;
        file["OldNotDeclaredHere"]["Type"] = "U32";
        file["OldNotDeclaredHere"]["Value"] = 3;
        writeSettingsFile(file);

        std::map<std::string, std::string> renames = {
            { "OldU32", "NewU32" }, { "OldString", "NewString" }, { "OldKeptBool", "NewKeptBool" },
            { "OldAlreadySet", "NewAlreadySet" }, { "OldTypeChanged", "NewTypeChanged" },
            { "OldVec3", "NewVec3" }, { "OldNotPersisted", "NewNotPersisted" },
            { "OldNotDeclaredHere", "NewNotDeclaredHere" }, { "OldAbsent", "NewU32" },
        };

        LLSD raw;
        ensure("raw settings read", LLControlGroup::readSettingsFile(mTestConfigFile, raw));
        U32 migrated = mCG->migrateRenamedControls(raw, renames, true, "test");
        ensure_equals("values carried over", migrated, 3U);
        ensure_equals("U32 carried over", mCG->getU32("NewU32"), 7U);
        ensure_equals("string carried over", mCG->getString("NewString"), std::string("hello"));
        ensure("vector carried over", mCG->getVector3("NewVec3") == LLVector3(1.f, 2.f, 3.f));
        ensure("new name in the file wins", !mCG->getBOOL("NewKeptBool"));
        ensure_equals("value already set is kept", mCG->getS32("NewAlreadySet"), 5);
        ensure_equals("type mismatch skipped", mCG->getU32("NewTypeChanged"), 0U);
        ensure("not persisted skipped", !mCG->getBOOL("NewNotPersisted"));
        ensure("unknown new name not declared", !mCG->controlExists("NewNotDeclaredHere"));

        // Carried-over values are saved under the new name.
        std::string saved_file = mTestConfigDir + "setting_migrated_temp.xml";
        mCleanups.push_back(saved_file);
        mCG->saveToFile(saved_file, true);
        LLSD saved;
        ensure("saved settings read", LLControlGroup::readSettingsFile(saved_file, saved));
        ensure("migrated value saved", saved.has("NewU32") && saved["NewU32"]["Value"].asInteger() == 7);

        // Idempotent: a second run changes nothing.
        ensure_equals("second run", mCG->migrateRenamedControls(raw, renames, true, "test"), 0U);
        ensure_equals("U32 unchanged", mCG->getU32("NewU32"), 7U);

        // Presets (only_if_default = false): the preset's value applies over the current one.
        LLSD preset;
        preset["OldAlreadySet"]["Type"] = "S32";
        preset["OldAlreadySet"]["Value"] = 11;
        ensure_equals("preset carried over", mCG->migrateRenamedControls(preset, renames, false, "preset"), 1U);
        ensure_equals("preset value applied", mCG->getS32("NewAlreadySet"), 11);

        LLSD missing;
        ensure("missing file", !LLControlGroup::readSettingsFile(mTestConfigDir + "no-such-file.xml", missing));
    }
}
