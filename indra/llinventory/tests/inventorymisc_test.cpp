/**
 * @file inventory.cpp
 * @author Phoenix
 * @date 2005-11-15
 * @brief Functions for inventory test framework
 *
 * $LicenseInfo:firstyear=2005&license=viewerlgpl$
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
#include "llsd.h"
#include "llsdserialize.h"

#include "../llinventory.h"
#include "../llsettingssky.h"
#include "llhazesky.h"
#include <limits>
#include "../test/lltut.h"


#if LL_WINDOWS
// disable unreachable code warnings
#pragma warning(disable: 4702)
#endif

void set_random_inventory_metadata(LLInventoryObject* obj)
{
    S32 extra = rand() % 4;
    switch (extra)
    {
    case 0:
    {
        LLUUID thumbnail_id;
        thumbnail_id.generate();
        obj->setThumbnailUUID(thumbnail_id);
        break;
    }
    case 1:
        obj->setFavorite(true);
        break;
    case 2:
    {
        LLUUID thumbnail_id;
        thumbnail_id.generate();
        obj->setThumbnailUUID(thumbnail_id);
        obj->setFavorite(true);
        break;
    }
    default:
        break;
    }
}

LLPointer<LLInventoryItem> create_random_inventory_item()
{
    LLUUID item_id;
    item_id.generate();
    LLUUID parent_id;
    parent_id.generate();
    LLPermissions perm;
    LLUUID creator_id;
    creator_id.generate();
    LLUUID owner_id;
    owner_id.generate();
    LLUUID last_owner_id;
    last_owner_id.generate();
    LLUUID group_id;
    group_id.generate();
    perm.init(creator_id, owner_id, last_owner_id, group_id);
    perm.initMasks(PERM_ALL, PERM_ALL, PERM_COPY, PERM_COPY, PERM_MODIFY | PERM_COPY);
    LLUUID asset_id;
    asset_id.generate();
    S32 price = rand();
    LLSaleInfo sale_info(LLSaleInfo::FS_COPY, price);
    U32 flags = rand();
    S32 creation = (S32)time(NULL);

    LLPointer<LLInventoryItem> item = new LLInventoryItem(
        item_id,
        parent_id,
        perm,
        asset_id,
        LLAssetType::AT_OBJECT,
        LLInventoryType::IT_ATTACHMENT,
        std::string("Sample Object"),
        std::string("Used for Testing"),
        sale_info,
        flags,
        creation);
    set_random_inventory_metadata(item);
    return item;
}

LLPointer<LLInventoryCategory> create_random_inventory_cat()
{
    LLUUID item_id;
    item_id.generate();
    LLUUID parent_id;
    parent_id.generate();

    LLPointer<LLInventoryCategory> cat = new LLInventoryCategory(
        item_id,
        parent_id,
        LLFolderType::FT_NONE,
        std::string("Sample category"));
    set_random_inventory_metadata(cat);
    return cat;
}

namespace tut
{
    struct inventory_data
    {
    };
    typedef test_group<inventory_data> inventory_test;
    typedef inventory_test::object inventory_object;
    tut::inventory_test inv("LLInventory");

//***class LLInventoryType***//


    template<> template<>
    void inventory_object::test<1>()
    {
        LLInventoryType::EType retType =  LLInventoryType::lookup(std::string("sound"));
        ensure("1.LLInventoryType::lookup(char*) failed", retType == LLInventoryType::IT_SOUND);

        retType = LLInventoryType::lookup(std::string("snapshot"));
        ensure("2.LLInventoryType::lookup(char*) failed", retType == LLInventoryType::IT_SNAPSHOT);
    }

    template<> template<>
    void inventory_object::test<2>()
    {
        static std::string retType =  LLInventoryType::lookup(LLInventoryType::IT_CALLINGCARD);
        ensure("1.LLInventoryType::lookup(EType) failed", (retType == "callcard"));

        retType = LLInventoryType::lookup(LLInventoryType::IT_LANDMARK);
        ensure("2.LLInventoryType::lookup(EType) failed", (retType == "landmark"));

    }

    template<> template<>
    void inventory_object::test<3>()
    {
        static std::string retType =  LLInventoryType::lookupHumanReadable(LLInventoryType::IT_CALLINGCARD);
        ensure("1.LLInventoryType::lookupHumanReadable(EType) failed", (retType == "calling card"));

        retType =  LLInventoryType::lookupHumanReadable(LLInventoryType::IT_LANDMARK);
        ensure("2.LLInventoryType::lookupHumanReadable(EType) failed", (retType == "landmark"));
    }

    template<> template<>
    void inventory_object::test<4>()
    {
        static LLInventoryType::EType retType =  LLInventoryType::defaultForAssetType(LLAssetType::AT_TEXTURE);
        ensure("1.LLInventoryType::defaultForAssetType(LLAssetType EType) failed", retType == LLInventoryType::IT_TEXTURE);

        retType =  LLInventoryType::defaultForAssetType(LLAssetType::AT_LANDMARK);
        ensure("2.LLInventoryType::defaultForAssetType(LLAssetType EType) failed", retType == LLInventoryType::IT_LANDMARK);
    }

//*****class LLInventoryItem*****//

    template<> template<>
    void inventory_object::test<5>()
    {
        LLPointer<LLInventoryItem> src = create_random_inventory_item();
        LLSD sd = ll_create_sd_from_inventory_item(src);
        //LL_INFOS() << "sd: " << *sd << LL_ENDL;
        LLPointer<LLInventoryItem> dst = new LLInventoryItem;
        bool successful_parse = dst->fromLLSD(sd);
        ensure_equals("0.LLInventoryItem::fromLLSD()", successful_parse, true);
        ensure_equals("1.item id::getUUID() failed", dst->getUUID(), src->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", dst->getParentUUID(), src->getParentUUID());
        ensure_equals("3.name::getName() failed", dst->getName(), src->getName());
        ensure_equals("4.type::getType() failed", dst->getType(), src->getType());

        ensure_equals("5.permissions::getPermissions() failed", dst->getPermissions(), src->getPermissions());
        ensure_equals("6.description::getDescription() failed", dst->getDescription(), src->getDescription());
        ensure_equals("7.sale type::getSaleType() failed", dst->getSaleInfo().getSaleType(), src->getSaleInfo().getSaleType());
        ensure_equals("8.sale price::getSalePrice() failed", dst->getSaleInfo().getSalePrice(), src->getSaleInfo().getSalePrice());
        ensure_equals("9.asset id::getAssetUUID() failed", dst->getAssetUUID(), src->getAssetUUID());
        ensure_equals("10.inventory type::getInventoryType() failed", dst->getInventoryType(), src->getInventoryType());
        ensure_equals("11.flags::getFlags() failed", dst->getFlags(), src->getFlags());
        ensure_equals("12.creation::getCreationDate() failed", dst->getCreationDate(), src->getCreationDate());

        LLUUID new_item_id, new_parent_id;
        new_item_id.generate();
        src->setUUID(new_item_id);

        new_parent_id.generate();
        src->setParent(new_parent_id);

        std::string new_name = "LindenLab";
        src->rename(new_name);

        src->setType(LLAssetType::AT_SOUND);

        LLUUID new_asset_id;
        new_asset_id.generate();

        src->setAssetUUID(new_asset_id);
        std::string new_desc = "SecondLife Testing";
        src->setDescription(new_desc);

        S32 new_price = rand();
        LLSaleInfo new_sale_info(LLSaleInfo::FS_COPY, new_price);
        src->setSaleInfo(new_sale_info);

        U32 new_flags = rand();
        S32 new_creation = (S32)time(NULL);

        LLPermissions new_perm;

        LLUUID new_creator_id;
        new_creator_id.generate();

        LLUUID new_owner_id;
        new_owner_id.generate();

        LLUUID last_owner_id;
        last_owner_id.generate();

        LLUUID new_group_id;
        new_group_id.generate();
        new_perm.init(new_creator_id, new_owner_id, last_owner_id, new_group_id);
        new_perm.initMasks(PERM_ALL, PERM_ALL, PERM_COPY, PERM_COPY, PERM_MODIFY | PERM_COPY);
        src->setPermissions(new_perm);

        src->setInventoryType(LLInventoryType::IT_SOUND);
        src->setFlags(new_flags);
        src->setCreationDate(new_creation);

        sd = ll_create_sd_from_inventory_item(src);
        //LL_INFOS() << "sd: " << *sd << LL_ENDL;
        successful_parse = dst->fromLLSD(sd);
        ensure_equals("13.item id::getUUID() failed", dst->getUUID(), src->getUUID());
        ensure_equals("14.parent::getParentUUID() failed", dst->getParentUUID(), src->getParentUUID());
        ensure_equals("15.name::getName() failed", dst->getName(), src->getName());
        ensure_equals("16.type::getType() failed", dst->getType(), src->getType());

        ensure_equals("17.permissions::getPermissions() failed", dst->getPermissions(), src->getPermissions());
        ensure_equals("18.description::getDescription() failed", dst->getDescription(), src->getDescription());
        ensure_equals("19.sale type::getSaleType() failed type", dst->getSaleInfo().getSaleType(), src->getSaleInfo().getSaleType());
        ensure_equals("20.sale price::getSalePrice() failed price", dst->getSaleInfo().getSalePrice(), src->getSaleInfo().getSalePrice());
        ensure_equals("21.asset id::getAssetUUID() failed id", dst->getAssetUUID(), src->getAssetUUID());
        ensure_equals("22.inventory type::getInventoryType() failed type", dst->getInventoryType(), src->getInventoryType());
        ensure_equals("23.flags::getFlags() failed", dst->getFlags(), src->getFlags());
        ensure_equals("24.creation::getCreationDate() failed", dst->getCreationDate(), src->getCreationDate());

    }

    template<> template<>
    void inventory_object::test<6>()
    {
        LLPointer<LLInventoryItem> src = create_random_inventory_item();

        LLUUID new_item_id, new_parent_id;
        new_item_id.generate();
        src->setUUID(new_item_id);

        new_parent_id.generate();
        src->setParent(new_parent_id);

        std::string new_name = "LindenLab";
        src->rename(new_name);

        src->setType(LLAssetType::AT_SOUND);

        LLUUID new_asset_id;
        new_asset_id.generate();

        src->setAssetUUID(new_asset_id);
        std::string new_desc = "SecondLife Testing";
        src->setDescription(new_desc);

        S32 new_price = rand();
        LLSaleInfo new_sale_info(LLSaleInfo::FS_COPY, new_price);
        src->setSaleInfo(new_sale_info);

        U32 new_flags = rand();
        S32 new_creation = (S32)time(NULL);

        LLPermissions new_perm;

        LLUUID new_creator_id;
        new_creator_id.generate();

        LLUUID new_owner_id;
        new_owner_id.generate();

        LLUUID last_owner_id;
        last_owner_id.generate();

        LLUUID new_group_id;
        new_group_id.generate();
        new_perm.init(new_creator_id, new_owner_id, last_owner_id, new_group_id);
        new_perm.initMasks(PERM_ALL, PERM_ALL, PERM_COPY, PERM_COPY, PERM_MODIFY | PERM_COPY);
        src->setPermissions(new_perm);

        src->setInventoryType(LLInventoryType::IT_SOUND);
        src->setFlags(new_flags);
        src->setCreationDate(new_creation);

        // test a save/load cycle to LLSD and back again
        // Note: ll_create_sd_from_inventory_item does not support metadata
        LLSD sd = ll_create_sd_from_inventory_item(src);
        LLPointer<LLInventoryItem> dst = new LLInventoryItem;
        bool successful_parse = dst->fromLLSD(sd);
        ensure_equals("0.LLInventoryItem::fromLLSD()", successful_parse, true);

        LLPointer<LLInventoryItem> src1 = create_random_inventory_item();
        src1->copyItem(src);

        ensure_equals("1.item id::getUUID() failed", dst->getUUID(), src1->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", dst->getParentUUID(), src1->getParentUUID());
        ensure_equals("3.name::getName() failed", dst->getName(), src1->getName());
        ensure_equals("4.type::getType() failed", dst->getType(), src1->getType());

        ensure_equals("5.permissions::getPermissions() failed", dst->getPermissions(), src1->getPermissions());
        ensure_equals("6.description::getDescription() failed", dst->getDescription(), src1->getDescription());
        ensure_equals("7.sale type::getSaleType() failed type", dst->getSaleInfo().getSaleType(), src1->getSaleInfo().getSaleType());
        ensure_equals("8.sale price::getSalePrice() failed price", dst->getSaleInfo().getSalePrice(), src1->getSaleInfo().getSalePrice());
        ensure_equals("9.asset id::getAssetUUID() failed id", dst->getAssetUUID(), src1->getAssetUUID());
        ensure_equals("10.inventory type::getInventoryType() failed type", dst->getInventoryType(), src1->getInventoryType());
        ensure_equals("11.flags::getFlags() failed", dst->getFlags(), src1->getFlags());
        ensure_equals("12.creation::getCreationDate() failed", dst->getCreationDate(), src1->getCreationDate());

        // quick test to make sure generateUUID() really works
        src1->generateUUID();
        ensure_not_equals("13.item id::generateUUID() failed", src->getUUID(), src1->getUUID());
    }

    template<> template<>
    void inventory_object::test<7>()
    {
        std::string filename("linden_file.dat");
        llofstream fileXML(filename.c_str());
        if (!fileXML.is_open())
        {
            LL_ERRS() << "file could not be opened\n" << LL_ENDL;
            return;
        }

        LLPointer<LLInventoryItem> src1 = create_random_inventory_item();
        LLSD sd;
        src1->asLLSD(sd);
        fileXML << LLSDOStreamer<LLSDNotationFormatter>(sd) << std::endl;
        fileXML.close();


        LLPointer<LLInventoryItem> src2 = new LLInventoryItem();
        llifstream file(filename.c_str());
        if (!file.is_open())
        {
            LL_ERRS() << "file could not be opened\n" << LL_ENDL;
            return;
        }
        std::string line;
        LLPointer<LLSDParser> parser = new LLSDNotationParser();
        std::getline(file, line);
        LLSD s_item;
        std::istringstream iss(line);
        if (parser->parse(iss, s_item, line.length()) == LLSDParser::PARSE_FAILURE)
        {
            LL_ERRS()<< "Parsing cache failed" << LL_ENDL;
            return;
        }
        src2->fromLLSD(s_item);

        file.close();

        ensure_equals("1.item id::getUUID() failed", src1->getUUID(), src2->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", src1->getParentUUID(), src2->getParentUUID());
        ensure_equals("3.permissions::getPermissions() failed", src1->getPermissions(), src2->getPermissions());
        ensure_equals("4.sale price::getSalePrice() failed price", src1->getSaleInfo().getSalePrice(), src2->getSaleInfo().getSalePrice());
        ensure_equals("5.asset id::getAssetUUID() failed id", src1->getAssetUUID(), src2->getAssetUUID());
        ensure_equals("6.type::getType() failed", src1->getType(), src2->getType());
        ensure_equals("7.inventory type::getInventoryType() failed type", src1->getInventoryType(), src2->getInventoryType());
        ensure_equals("8.name::getName() failed", src1->getName(), src2->getName());
        ensure_equals("9.description::getDescription() failed", src1->getDescription(), src2->getDescription());
        ensure_equals("10.creation::getCreationDate() failed", src1->getCreationDate(), src2->getCreationDate());
        ensure_equals("13.thumbnails::getThumbnailUUID() failed", src1->getThumbnailUUID(), src2->getThumbnailUUID());
        ensure_equals("14.favorites::getIsFavorite() failed", src1->getIsFavorite(), src2->getIsFavorite());
    }

    template<> template<>
    void inventory_object::test<8>()
    {
        LLPointer<LLInventoryItem> src1 = create_random_inventory_item();

        std::ostringstream ostream;
        src1->exportLegacyStream(ostream, true);

        std::istringstream istream(ostream.str());
        LLPointer<LLInventoryItem> src2 = new LLInventoryItem();
        src2->importLegacyStream(istream);

        ensure_equals("1.item id::getUUID() failed", src1->getUUID(), src2->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", src1->getParentUUID(), src2->getParentUUID());
        ensure_equals("3.permissions::getPermissions() failed", src1->getPermissions(), src2->getPermissions());
        ensure_equals("4.sale price::getSalePrice() failed price", src1->getSaleInfo().getSalePrice(), src2->getSaleInfo().getSalePrice());
        ensure_equals("5.asset id::getAssetUUID() failed id", src1->getAssetUUID(), src2->getAssetUUID());
        ensure_equals("6.type::getType() failed", src1->getType(), src2->getType());
        ensure_equals("7.inventory type::getInventoryType() failed type", src1->getInventoryType(), src2->getInventoryType());
        ensure_equals("8.name::getName() failed", src1->getName(), src2->getName());
        ensure_equals("9.description::getDescription() failed", src1->getDescription(), src2->getDescription());
        ensure_equals("10.creation::getCreationDate() failed", src1->getCreationDate(), src2->getCreationDate());
        ensure_equals("11.thumbnails::getThumbnailUUID() failed", src1->getThumbnailUUID(), src2->getThumbnailUUID());
        ensure_equals("12.favorites::getIsFavorite() failed", false, src2->getIsFavorite()); // not supposed to carry over
    }

    template<> template<>
    void inventory_object::test<9>()
    {
        // Deleted LLInventoryItem::exportFileXML() and LLInventoryItem::importXML()
        // because I can't find any non-test code references to it. 2009-05-04 JC
    }

    template<> template<>
    void inventory_object::test<11>()
    {
        LLPointer<LLInventoryItem> src1 = create_random_inventory_item();
        LLSD retSd = src1->asLLSD();
        LLPointer<LLInventoryItem> src2 = new LLInventoryItem();
        src2->fromLLSD(retSd);

        ensure_equals("1.item id::getUUID() failed", src1->getUUID(), src2->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", src1->getParentUUID(), src2->getParentUUID());
        ensure_equals("3.permissions::getPermissions() failed", src1->getPermissions(), src2->getPermissions());
        ensure_equals("4.asset id::getAssetUUID() failed id", src1->getAssetUUID(), src2->getAssetUUID());
        ensure_equals("5.type::getType() failed", src1->getType(), src2->getType());
        ensure_equals("6.inventory type::getInventoryType() failed type", src1->getInventoryType(), src2->getInventoryType());
        ensure_equals("7.flags::getFlags() failed", src1->getFlags(), src2->getFlags());
        ensure_equals("8.sale type::getSaleType() failed type", src1->getSaleInfo().getSaleType(), src2->getSaleInfo().getSaleType());
        ensure_equals("9.sale price::getSalePrice() failed price", src1->getSaleInfo().getSalePrice(), src2->getSaleInfo().getSalePrice());
        ensure_equals("10.name::getName() failed", src1->getName(), src2->getName());
        ensure_equals("11.description::getDescription() failed", src1->getDescription(), src2->getDescription());
        ensure_equals("12.creation::getCreationDate() failed", src1->getCreationDate(), src2->getCreationDate());
        ensure_equals("13.thumbnails::getThumbnailUUID() failed", src1->getThumbnailUUID(), src2->getThumbnailUUID());
        ensure_equals("14.favorites::getIsFavorite() failed", src1->getIsFavorite(), src2->getIsFavorite());
    }

//******class LLInventoryCategory*******//

    template<> template<>
    void inventory_object::test<12>()
    {
        LLPointer<LLInventoryCategory> src = create_random_inventory_cat();
        LLSD sd = ll_create_sd_from_inventory_category(src);
        LLPointer<LLInventoryCategory> dst = ll_create_category_from_sd(sd);

        ensure_equals("1.item id::getUUID() failed", dst->getUUID(), src->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", dst->getParentUUID(), src->getParentUUID());
        ensure_equals("3.name::getName() failed", dst->getName(), src->getName());
        ensure_equals("4.type::getType() failed", dst->getType(), src->getType());
        ensure_equals("5.preferred type::getPreferredType() failed", dst->getPreferredType(), src->getPreferredType());

        src->setPreferredType( LLFolderType::FT_TEXTURE);
        sd = ll_create_sd_from_inventory_category(src);
        dst = ll_create_category_from_sd(sd);
        ensure_equals("6.preferred type::getPreferredType() failed", dst->getPreferredType(), src->getPreferredType());


    }

    template<> template<>
    void inventory_object::test<13>()
    {
        std::string filename("linden_file.dat");
        llofstream fileXML(filename.c_str());
        if (!fileXML.is_open())
        {
            LL_ERRS() << "file could not be opened\n" << LL_ENDL;
            return;
        }

        LLPointer<LLInventoryCategory> src1 = create_random_inventory_cat();
        LLSD sd;
        src1->exportLLSD(sd);
        fileXML << LLSDOStreamer<LLSDNotationFormatter>(sd) << std::endl;
        fileXML.close();

        llifstream file(filename.c_str());
        if (!file.is_open())
        {
            LL_ERRS() << "file could not be opened\n" << LL_ENDL;
            return;
        }
        std::string line;
        LLPointer<LLSDParser> parser = new LLSDNotationParser();
        std::getline(file, line);
        LLSD s_item;
        std::istringstream iss(line);
        if (parser->parse(iss, s_item, line.length()) == LLSDParser::PARSE_FAILURE)
        {
            LL_ERRS()<< "Parsing cache failed" << LL_ENDL;
            return;
        }

        file.close();

        LLPointer<LLInventoryCategory> src2 = new LLInventoryCategory();
        src2->importLLSDMap(s_item);

        ensure_equals("1.item id::getUUID() failed", src1->getUUID(), src2->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", src1->getParentUUID(), src2->getParentUUID());
        ensure_equals("3.type::getType() failed", src1->getType(), src2->getType());
        ensure_equals("4.preferred type::getPreferredType() failed", src1->getPreferredType(), src2->getPreferredType());
        ensure_equals("5.name::getName() failed", src1->getName(), src2->getName());
        ensure_equals("6.thumbnails::getThumbnailUUID() failed", src1->getThumbnailUUID(), src2->getThumbnailUUID());
        ensure_equals("7.favorites::getIsFavorite() failed", src1->getIsFavorite(), src2->getIsFavorite());
    }

    template<> template<>
    void inventory_object::test<14>()
    {
        LLPointer<LLInventoryCategory> src1 = create_random_inventory_cat();

        std::ostringstream ostream;
        src1->exportLegacyStream(ostream, true);

        std::istringstream istream(ostream.str());
        LLPointer<LLInventoryCategory> src2 = new LLInventoryCategory();
        src2->importLegacyStream(istream);

        ensure_equals("1.item id::getUUID() failed", src1->getUUID(), src2->getUUID());
        ensure_equals("2.parent::getParentUUID() failed", src1->getParentUUID(), src2->getParentUUID());
        ensure_equals("3.type::getType() failed", src1->getType(), src2->getType());
        ensure_equals("4.preferred type::getPreferredType() failed", src1->getPreferredType(), src2->getPreferredType());
        ensure_equals("5.name::getName() failed", src1->getName(), src2->getName());
        ensure_equals("13.thumbnails::getThumbnailUUID() failed", src1->getThumbnailUUID(), src2->getThumbnailUUID());
        ensure_equals("14.favorites::getIsFavorite() failed", false, src2->getIsFavorite()); // currently not supposed to carry over
    }
    // #656: an update map without a "permissions" key (AIS link replies) must keep the
    // item's existing permissions; one with the key replaces them completely.
    template<> template<>
    void inventory_object::test<15>()
    {
        LLPointer<LLInventoryItem> src = create_random_inventory_item();
        const LLPermissions orig_perm = src->getPermissions();

        LLSD sd = src->asLLSD();
        sd.erase("permissions");
        sd.erase("sale_info");
        sd["desc"] = "@1601";

        LLPointer<LLInventoryItem> dst = new LLInventoryItem;
        dst->copyItem(src);
        ensure("0.LLInventoryItem::fromLLSD()", dst->fromLLSD(sd));
        ensure_equals("1.desc updated", dst->getDescription(), std::string("@1601"));
        ensure_equals("2.owner kept", dst->getPermissions().getOwner(), orig_perm.getOwner());
        ensure_equals("3.creator kept", dst->getPermissions().getCreator(), orig_perm.getCreator());
        ensure_equals("4.owner mask kept", dst->getPermissions().getMaskOwner(), orig_perm.getMaskOwner());
        ensure_equals("5.everyone mask kept", dst->getPermissions().getMaskEveryone(), orig_perm.getMaskEveryone());

        LLSD perm_sd = LLSD::emptyMap();
        LLUUID new_owner;
        new_owner.generate();
        perm_sd["owner_id"] = new_owner;
        LLSD sd2 = src->asLLSD();
        sd2["permissions"] = perm_sd;
        sd2.erase("sale_info");
        ensure("6.LLInventoryItem::fromLLSD()", dst->fromLLSD(sd2));
        ensure_equals("7.owner replaced", dst->getPermissions().getOwner(), new_owner);
        ensure("8.creator reset to default", dst->getPermissions().getCreator().isNull());
    }
}

namespace tut
{
    struct haze_settings_data {};
    typedef test_group<haze_settings_data> haze_settings_test;
    typedef haze_settings_test::object haze_settings_object;
    haze_settings_test haze_settings("Haze sky settings");

    template<> template<>
    void haze_settings_object::test<1>()
    {
        const std::string &start = LLSettingsSky::SETTING_SL_HAZE_START_DISTANCE;
        const std::string &opacity = LLSettingsSky::SETTING_SL_HAZE_MAX_OPACITY;
        LLSD values = LLSD::emptyMap();
        LLSettingsSky::normalizeHazeGuardrails(values);
        ensure("absent fields remain absent", !values.has(start) && !values.has(opacity));
        values[start] = LLSD::Integer(600);
        values[opacity] = LLSD::Real(-0.1);
        LLSettingsSky::normalizeHazeGuardrails(values);
        ensure("integer converted to real", values[start].type() == LLSD::TypeReal);
        ensure_equals("start clamped", values[start].asReal(), 512.0);
        ensure_equals("opacity clamped", values[opacity].asReal(), 0.0);
        values[start] = "bad";
        values[opacity] = LLSD::Real(std::numeric_limits<F64>::infinity());
        LLSettingsSky::normalizeHazeGuardrails(values);
        ensure("bad values removed independently", !values.has(start) && !values.has(opacity));
    }

    template<> template<>
    void haze_settings_object::test<2>()
    {
        const std::string &start = LLSettingsSky::SETTING_SL_HAZE_START_DISTANCE;
        const std::string &opacity = LLSettingsSky::SETTING_SL_HAZE_MAX_OPACITY;
        for (bool partial : { false, true })
        {
            LLSD values = LLSettingsSky::defaults();
            values[start] = LLSD::Real(0.0);
            LLSD result = LLSettingsSky::validateSkySettings(values, partial);
            ensure("sky validation succeeds", result["success"].asBoolean());
            ensure("zero preserved", values.has(start) && values[start].asReal() == 0.0);
            ensure("automatic opacity remains absent", !values.has(opacity));
            std::ostringstream stream;
            LLSDSerialize::toXML(values, stream);
            std::istringstream input(stream.str());
            LLSD restored;
            LLSDSerialize::fromXML(restored, input);
            ensure("serialized presence retained", restored.has(start) && !restored.has(opacity));
            ensure("serialized value is real", restored[start].type() == LLSD::TypeReal);
        }
    }
}

namespace tut
{
    class TestHazeSky : public LLSettingsSky
    {
    public:
        explicit TestHazeSky(const LLSD &settings) : LLSettingsSky(settings) {}
        void installTransition(const LLHaze::Transition &transition) { mHazeTransition = transition; }
        ptr_t buildClone() override
        {
            auto clone = std::make_shared<TestHazeSky>(cloneSettings());
            clone->copyHazeTransitionFrom(*this);
            return clone;
        }
    };

    template<> template<>
    void haze_settings_object::test<3>()
    {
        const auto &start = LLSettingsSky::SETTING_SL_HAZE_START_DISTANCE;
        const auto &opacity = LLSettingsSky::SETTING_SL_HAZE_MAX_OPACITY;
        for (bool partial : { false, true })
        {
            LLSD values = LLSettingsSky::defaults();
            values[start] = LLSD::Integer(12);
            values[opacity] = LLSD::Real(std::numeric_limits<F64>::quiet_NaN());
            ensure("invalid optional key does not invalidate sky", LLSettingsSky::validateSkySettings(values, partial)["success"].asBoolean());
            ensure("integer normalized before validator", values[start].type() == LLSD::TypeReal && values[start].asReal() == 12.0);
            ensure("nonfinite optional key removed", !values.has(opacity));
        }
    }

    template<> template<>
    void haze_settings_object::test<4>()
    {
        const auto &start = LLSettingsSky::SETTING_SL_HAZE_START_DISTANCE;
        const auto &opacity = LLSettingsSky::SETTING_SL_HAZE_MAX_OPACITY;
        LLSD first = LLSettingsSky::defaults();
        LLSD last = LLSettingsSky::defaults();
        first[start] = LLSD::Real(10.0);
        last[start] = LLSD::Real(30.0);
        last[opacity] = LLSD::Real(0.8);
        auto sky = std::make_shared<TestHazeSky>(first);
        LLSettingsBase::ptr_t end = std::make_shared<TestHazeSky>(last);
        sky->blend(end, 0.25);
        const auto *transition = sky->getHazeTransition();
        ensure("transition exists", transition != nullptr);
        ensure("endpoint presence independent", !transition->from.saved.maxOpacity.present && transition->to.saved.maxOpacity.present);
        ensure_approximately_equals("manual endpoint blend", LLHaze::resolveTransition(*transition).startDistance.value, 15.0, 4);
        ensure("automatic value not baked", !sky->getSettings().has(opacity));
        auto clone = sky->buildClone();
        ensure("clone keeps metadata", clone->getHazeTransition() != nullptr);
        auto target = std::make_shared<TestHazeSky>(first);
        LLSettingsBase::ptr_t initial = clone;
        target->replaceSettings(initial);
        ensure("pointer replacement keeps transition for blender evaluation", target->getHazeTransition() != nullptr);
        LLSettingsBase::ptr_t target_base = target;
        LLSettingsBlender blender(target_base, initial, end);
        ensure("blender initialization preserves transition", target->getHazeTransition() != nullptr);
        blender.setBlendFactor(0.0);
        const auto first_blend = LLHaze::resolveTransition(*target->getHazeTransition());
        ensure_approximately_equals("blender begins at interrupted preview", first_blend.startDistance.value,
                                    LLHaze::resolveTransition(*clone->getHazeTransition()).startDistance.value, 16);
        blender.setBlendFactor(0.5);
        const auto midpoint = LLHaze::resolveTransition(*target->getHazeTransition());
        const auto last_endpoint = LLHaze::resolveEndpoint(target->getHazeTransition()->to);
        ensure_approximately_equals("blender evaluates from original frozen endpoint", midpoint.startDistance.value,
                                    (first_blend.startDistance.value + last_endpoint.startDistance.value) * .5, 16);
        const LLHaze::Pair before = LLHaze::resolveTransition(*clone->getHazeTransition());
        LLSettingsBase::ptr_t another = std::make_shared<TestHazeSky>(first);
        clone->blend(another, 0.0);
        const auto *interrupted = clone->getHazeTransition();
        ensure("interrupted transition is frozen", interrupted && interrupted->from.frozen);
        ensure_approximately_equals("interrupted start continuous", LLHaze::resolveTransition(*interrupted).startDistance.value, before.startDistance.value, 4);
        ensure_approximately_equals("interrupted opacity continuous", LLHaze::resolveTransition(*interrupted).maxOpacity.value, before.maxOpacity.value, 4);
        ensure("freeze not serialized", !clone->getSettings().has(opacity));
        clone->setValue(start, LLSD::Real(0.0));
        ensure("explicit edit clears metadata", !clone->getHazeTransition());
        clone->clearHazeGuardrail(start);
        ensure("automatic removes field", !clone->getSettings().has(start));
    }
}

namespace tut
{
    template<> template<>
    void haze_settings_object::test<5>()
    {
        struct RestorePolicy
        {
            bool original = LLHaze::illuminationPolicy().physicalNight;
            ~RestorePolicy() { LLHaze::illuminationPolicy().physicalNight = original; }
        } restore;

        // A night sky with a visible moon makes the two physical-night policies differ.
        LLHaze::illuminationPolicy().physicalNight = false;
        LLHaze::Endpoint raw;
        raw.hasLighting = true;
        raw.lighting.sun = {0., 0., -1.};
        raw.lighting.moon = {0., 0., 1.};
        raw.inputs.illuminationRatio = LLHaze::illumination(raw.lighting, false) / LLHaze::referenceIllumination();
        LLHaze::Endpoint frozen;
        frozen.frozen = true;
        frozen.snapshot.startDistance = {6., LLHaze::Source::Sky};
        frozen.snapshot.maxOpacity = {.7, LLHaze::Source::Sky};
        LLHaze::Transition old{raw, frozen, .4};
        const LLHaze::Pair stale = LLHaze::resolveTransition(old);
        LLHaze::illuminationPolicy().physicalNight = true;
        LLHaze::Transition refreshed = old;
        llHazeRefreshTransition(refreshed);
        const LLHaze::Pair expected = LLHaze::resolveTransition(refreshed);
        ensure("policy toggle changes unfrozen endpoint", std::abs(expected.startDistance.value - stale.startDistance.value) > .0001);
        ensure("refresh keeps frozen snapshot", refreshed.to.snapshot.startDistance.value == 6. && refreshed.to.snapshot.maxOpacity.value == .7);

        LLSD values = LLSettingsSky::defaults();
        const auto &start = LLSettingsSky::SETTING_SL_HAZE_START_DISTANCE;
        const auto &opacity = LLSettingsSky::SETTING_SL_HAZE_MAX_OPACITY;
        auto from = std::make_shared<TestHazeSky>(values);
        auto to = std::make_shared<TestHazeSky>(values);
        from->installTransition(old);
        to->installTransition(old);
        LLSettingsBase::ptr_t end = to;
        from->blend(end, .5);
        const LLHaze::Transition *current = from->getHazeTransition();
        ensure("both interrupted endpoints frozen", current && current->from.frozen && current->to.frozen);
        ensure_approximately_equals("source uses refreshed start", current->from.snapshot.startDistance.value, expected.startDistance.value, 20);
        ensure_approximately_equals("source uses refreshed opacity", current->from.snapshot.maxOpacity.value, expected.maxOpacity.value, 20);
        ensure_approximately_equals("destination uses refreshed start", current->to.snapshot.startDistance.value, expected.startDistance.value, 20);
        ensure_approximately_equals("destination uses refreshed opacity", current->to.snapshot.maxOpacity.value, expected.maxOpacity.value, 20);
        ensure("destination metadata was not modified", to->getHazeTransition()->from.inputs.illuminationRatio == old.from.inputs.illuminationRatio);
        ensure("automatic keys remain absent", !from->getSettings().has(start) && !from->getSettings().has(opacity));
    }
}

namespace tut
{
    template<> template<>
    void haze_settings_object::test<6>()
    {
        const auto &start = LLSettingsSky::SETTING_SL_HAZE_START_DISTANCE;
        const auto &opacity = LLSettingsSky::SETTING_SL_HAZE_MAX_OPACITY;
        LLSD values = LLSettingsSky::defaults();
        auto source = std::make_shared<TestHazeSky>(values);
        LLSettingsBase::ptr_t other = std::make_shared<TestHazeSky>(values);
        source->blend(other, .5);
        ensure("ordinary sky blend retains endpoint metadata", source->getHazeTransition() != nullptr);
        auto edited = source->buildClone();
        const LLHaze::Pair before = LLHaze::resolveTransition(*edited->getHazeTransition());
        edited->setHazeDensity(0.f);
        ensure("existing density edit clears blended metadata", !edited->getHazeTransition());
        const LLHaze::Endpoint endpoint = llHazeEndpoint(*edited);
        const LLHaze::Pair after = LLHaze::resolveEndpoint(endpoint);
        ensure("automatic resolves from edited density", endpoint.inputs.hazeDensity == 0. &&
               std::abs(after.maxOpacity.value - before.maxOpacity.value) > .0001);
        ensure("automatic values are not saved", !edited->getSettings().has(start) && !edited->getSettings().has(opacity));

        const auto check_edit = [&](const char *label, const auto &edit)
        {
            auto sky = source->buildClone();
            edit(*sky);
            ensure(label, !sky->getHazeTransition());
            ensure("edit preserves automatic presence", !sky->getSettings().has(start) && !sky->getSettings().has(opacity));
        };
        check_edit("density multiplier clears metadata", [](LLSettingsSky &sky) { sky.setDensityMultiplier(.0002f); });
        check_edit("distance multiplier clears metadata", [](LLSettingsSky &sky) { sky.setDistanceMultiplier(2.f); });
        check_edit("maximum altitude clears metadata", [](LLSettingsSky &sky) { sky.setMaxY(1000.f); });
        check_edit("ambient clears metadata", [](LLSettingsSky &sky) { sky.setAmbientColor(LLColor3(.3f, .4f, .5f)); });
        check_edit("blue density clears metadata", [](LLSettingsSky &sky) { sky.setBlueDensity(LLColor3(.2f, .3f, .4f)); });
        check_edit("sunlight clears metadata", [](LLSettingsSky &sky) { sky.setSunlightColor(LLColor3(.5f, .6f, .7f)); });
        check_edit("sun rotation clears metadata", [](LLSettingsSky &sky) { sky.setSunRotation(LLQuaternion()); });
        check_edit("moon rotation clears metadata", [](LLSettingsSky &sky) { sky.setMoonRotation(LLQuaternion()); });
        check_edit("moon brightness clears metadata", [](LLSettingsSky &sky) { sky.setMoonBrightness(.2f); });
    }

    template<> template<>
    void haze_settings_object::test<7>()
    {
        // Exercise the legacy ambient conversion inside blend: it must not look
        // like a creator edit and clear the newly constructed endpoints.
        LLSD left = LLSettingsSky::defaults();
        left.erase(LLSettingsSky::SETTING_LEGACY_HAZE);
        left[LLSettingsSky::SETTING_AMBIENT] = LLColor3(.2f, .3f, .4f).getValue();
        auto sky = std::make_shared<TestHazeSky>(left);
        LLSettingsBase::ptr_t end = std::make_shared<TestHazeSky>(LLSettingsSky::defaults());
        sky->blend(end, .5);
        ensure("legacy ambient blend retains transition metadata", sky->getHazeTransition() != nullptr);
    }
}
