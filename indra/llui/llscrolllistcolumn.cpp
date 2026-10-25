/**
 * @file llscrollcolumnheader.cpp
 * @brief Scroll lists are composed of rows (items), each of which
 * contains columns (cells).
 *
 * $LicenseInfo:firstyear=2007&license=viewerlgpl$
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

#include "llscrolllistcolumn.h"

#include "llbutton.h"
#include "llrender2dutils.h"
#include "llresizebar.h"
#include "llscrolllistcell.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lluictrlfactory.h"

const S32 MIN_COLUMN_WIDTH = 20;

// defaults for LLScrollColumnHeader param block pulled from widgets/scroll_column_header.xml
static LLWidgetNameRegistry::StaticRegistrar sRegisterColumnHeaderParams(&typeid(LLScrollColumnHeader::Params), "scroll_column_header");

//---------------------------------------------------------------------------
// LLScrollColumnHeader
//---------------------------------------------------------------------------
LLScrollColumnHeader::Params::Params()
:   column("column"),
    flat("flat", false),
    bg_color("bg_color", LLColor4::transparent),
    bg_hover_color("bg_hover_color", LLColor4::transparent),
    bg_pressed_color("bg_pressed_color", LLColor4::transparent),
    separator_color("separator_color", LLColor4::transparent),
    sort_arrow_color("sort_arrow_color", LLColor4::white),
    icon("icon"),
    icon_size("icon_size", 12)
{}


LLScrollColumnHeader::LLScrollColumnHeader(const LLScrollColumnHeader::Params& p)
:   LLButton(p), // use combobox params to steal images
    mColumn(p.column),
    mHasResizableElement(false),
    mFlat(p.flat),
    mBgColor(p.bg_color()),
    mBgHoverColor(p.bg_hover_color()),
    mBgPressedColor(p.bg_pressed_color()),
    mSeparatorColor(p.separator_color()),
    mSortArrowColor(p.sort_arrow_color()),
    mIcon(p.icon()),
    mIconSize(p.icon_size)
{
    if (mIcon.notNull() && !mFlat)
    {
        // classic header: the icon rides the overlay slot (draw() leaves it alone)
        setImageOverlay(mIcon->getName(), LLFontGL::HCENTER, LLColor4::white);
    }

    setClickedCallback(boost::bind(&LLScrollColumnHeader::onClick, this, _2));

    // resize handles on left and right
    const S32 RESIZE_BAR_THICKNESS = 3;
    LLResizeBar::Params resize_bar_p;
    resize_bar_p.resizing_view(this);
    resize_bar_p.rect(LLRect(getRect().getWidth() - RESIZE_BAR_THICKNESS, getRect().getHeight(), getRect().getWidth(), 0));
    resize_bar_p.min_size(MIN_COLUMN_WIDTH);
    resize_bar_p.side(LLResizeBar::RIGHT);
    resize_bar_p.enabled(false);
    mResizeBar = LLUICtrlFactory::create<LLResizeBar>(resize_bar_p);
    addChild(mResizeBar);
}

LLScrollColumnHeader::~LLScrollColumnHeader()
{}

void LLScrollColumnHeader::draw()
{
    if (mFlat)
    {
        drawFlat();
        return;
    }

    if (mIcon.notNull())
    {
        // icon headers keep their icon in the overlay slot; no arrow (as for unlabeled columns)
        LLButton::draw();
        return;
    }

    std::string sort_column = mColumn->mParentCtrl->getSortColumnName();
    bool draw_arrow = !mColumn->mLabel.empty()
            && mColumn->mParentCtrl->isSorted()
            // check for indirect sorting column as well as column's sorting name
            && (sort_column == mColumn->mSortingColumn || sort_column == mColumn->mName);

    bool is_ascending = mColumn->mParentCtrl->getSortAscending();
    if (draw_arrow)
    {
        setImageOverlay(is_ascending ? "up_arrow.tga" : "down_arrow.tga", LLFontGL::RIGHT, LLColor4::white);
    }
    else
    {
        setImageOverlay(LLUUID::null);
    }

    // Draw children
    LLButton::draw();
}

void LLScrollColumnHeader::drawFlat()
{
    const S32 width = getRect().getWidth();
    const S32 height = getRect().getHeight();
    const S32 column_width = llmin(mColumn->getWidth(), width);
    if (column_width <= 0 || height <= 0)
    {
        LLUICtrl::draw();
        return;
    }

    const F32 alpha = getCurrentTransparency();
    const bool enabled = isInEnabledChain();

    bool pressed = false;
    if (hasMouseCapture())
    {
        S32 local_mouse_x, local_mouse_y;
        LLUI::getInstance()->getMousePositionLocal(this, &local_mouse_x, &local_mouse_y);
        pressed = pointInView(local_mouse_x, local_mouse_y);
    }

    LLScrollListCtrl* list = mColumn->mParentCtrl;
    const std::string sort_column = list->getSortColumnName();
    const bool sorted = mColumn->mCanSort && list->hasSortOrder()
        && (sort_column == mColumn->mSortingColumn || sort_column == mColumn->mName);
    const bool ascending = list->getSortAscending();

    // Plain fill, a 1px rule along the bottom and a short divider in the gutter after the column
    const LLColor4 bg = pressed && enabled ? mBgPressedColor.get()
                      : (mNeedsHighlight && enabled ? mBgHoverColor.get() : mBgColor.get());
    if (bg.mV[VALPHA] > 0.f)
    {
        gl_rect_2d(0, height, width, 0, bg % alpha, true);
    }
    const LLColor4 separator = mSeparatorColor.get() % alpha;
    if (separator.mV[VALPHA] > 0.f)
    {
        gl_rect_2d(0, 1, width, 0, separator, true);
        if (width > column_width)
        {
            const S32 divider_x = column_width + (width - column_width) / 2;
            const S32 inset = llmin(4, height / 4);
            gl_rect_2d(divider_x, height - inset, divider_x + 1, inset, separator, true);
        }
    }

    LLColor4 text_color = !enabled ? mDisabledLabelColor.get()
                        : (sorted ? mSelectedLabelColor.get() : mUnselectedLabelColor.get());
    text_color.mV[VALPHA] *= alpha;

    // Content sits inside the column itself (not the gutter) so it lines up with the cells
    const S32 PAD = 2;
    const S32 content_left = PAD;
    const S32 content_right = column_width - (mHAlign == LLFontGL::RIGHT ? 1 : PAD);
    const S32 avail = llmax(0, content_right - content_left);

    const LLFontGL* font = getFont();
    LLWString label = mColumn->mLabel.getWString();
    LLWStringUtil::trim(label);
    S32 content_width = 0;
    if (mIcon.notNull())
    {
        content_width = llmin(mIconSize, llmin(avail, height - 4));
    }
    else if (!label.empty() && font)
    {
        content_width = llmin(avail, (S32)font->getWidth(label.c_str()));
    }

    // Sort triangle: beside the content when it fits, otherwise a smaller one underneath
    const S32 ARROW_W = 7, ARROW_H = 4, ARROW_GAP = 3;
    const S32 SMALL_ARROW_W = 5, SMALL_ARROW_H = 3;
    const bool inline_arrow = sorted && (content_width + ARROW_GAP + ARROW_W <= avail);
    const bool stacked_arrow = sorted && !inline_arrow;
    const S32 group_width = content_width + (inline_arrow ? ARROW_GAP + ARROW_W : 0);

    S32 group_left;
    switch (mHAlign)
    {
        case LLFontGL::RIGHT:
            group_left = content_right - group_width;
            break;
        case LLFontGL::HCENTER:
            group_left = content_left + (avail - group_width) / 2;
            break;
        case LLFontGL::LEFT:
        default:
            group_left = content_left;
            break;
    }
    // Right-aligned columns put the arrow before the label so the label's edge stays on the cells'
    const bool arrow_first = inline_arrow && mHAlign == LLFontGL::RIGHT;
    const S32 content_x = arrow_first ? group_left + ARROW_W + ARROW_GAP : group_left;
    const S32 center_y = height / 2 + (stacked_arrow ? 2 : 0);

    if (mIcon.notNull() && content_width > 0)
    {
        mIcon->draw(content_x, center_y - content_width / 2, content_width, content_width, text_color);
    }
    else if (content_width > 0)
    {
        font->render(label, 0, (F32)content_x, (F32)center_y, text_color,
                     LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                     S32_MAX, avail, NULL, true);
    }

    if (sorted)
    {
        LLColor4 arrow_color = mSortArrowColor.get();
        arrow_color.mV[VALPHA] *= alpha;
        S32 aw, ah, ax, ay;
        if (inline_arrow)
        {
            aw = ARROW_W;
            ah = ARROW_H;
            ax = arrow_first ? group_left : content_x + content_width + ARROW_GAP;
            ay = height / 2 - ah / 2 - 1;
        }
        else
        {
            aw = SMALL_ARROW_W;
            ah = SMALL_ARROW_H;
            ax = (content_width > 0 ? content_x + content_width / 2 : content_left + avail / 2) - aw / 2;
            ay = 2;
        }
        if (ascending)
        {
            gl_triangle_2d(ax, ay, ax + aw, ay, ax + aw / 2, ay + ah, arrow_color, true);
        }
        else
        {
            gl_triangle_2d(ax, ay + ah, ax + aw, ay + ah, ax + aw / 2, ay, arrow_color, true);
        }
    }

    // children (the column resize bar)
    LLUICtrl::draw();
}

bool LLScrollColumnHeader::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    if (canResize() && mResizeBar->getRect().pointInRect(x, y))
    {
        // reshape column to max content width
        mColumn->mParentCtrl->calcMaxContentWidth();
        LLRect column_rect = getRect();
        column_rect.mRight = column_rect.mLeft + mColumn->mMaxContentWidth;
        setShape(column_rect, true);
    }
    else
    {
        onClick(LLSD());
    }
    return true;
}

void LLScrollColumnHeader::onClick(const LLSD& data)
{
//  if (mColumn)
// [SL:KB] - Patch: Control-ScrollList | Checked: Catznip-5.2
    if ( (mColumn) && (mColumn->mCanSort) )
// [/SL:KB]
    {
        LLScrollListCtrl::onClickColumn(mColumn);
    }
}

LLView* LLScrollColumnHeader::findSnapEdge(S32& new_edge_val, const LLCoordGL& mouse_dir, ESnapEdge snap_edge, ESnapType snap_type, S32 threshold, S32 padding)
{
    // this logic assumes dragging on right
    llassert(snap_edge == SNAP_RIGHT);

    // use higher snap threshold for column headers
    threshold = llmin(threshold, 10);

    LLRect snap_rect = getSnapRect();

    mColumn->mParentCtrl->calcMaxContentWidth();

    S32 snap_delta = mColumn->mMaxContentWidth - snap_rect.getWidth();

    // x coord growing means column growing, so same signs mean we're going in right direction
    if (llabs(snap_delta) <= threshold && mouse_dir.mX * snap_delta > 0 )
    {
        new_edge_val = snap_rect.mRight + snap_delta;
    }
    else
    {
        LLScrollListColumn* next_column = mColumn->mParentCtrl->getColumn(mColumn->mIndex + 1);
        while (next_column)
        {
            if (next_column->mHeader)
            {
                snap_delta = (next_column->mHeader->getSnapRect().mRight - next_column->mMaxContentWidth) - snap_rect.mRight;
                if (llabs(snap_delta) <= threshold && mouse_dir.mX * snap_delta > 0 )
                {
                    new_edge_val = snap_rect.mRight + snap_delta;
                }
                break;
            }
            next_column = mColumn->mParentCtrl->getColumn(next_column->mIndex + 1);
        }
    }

    return this;
}

void LLScrollColumnHeader::handleReshape(const LLRect& new_rect, bool by_user)
{
    S32 new_width = new_rect.getWidth();
    S32 delta_width = new_width - (getRect().getWidth() /*+ mColumn->mParentCtrl->getColumnPadding()*/);

    if (delta_width != 0)
    {
        S32 remaining_width = -delta_width;
        S32 col;
        for (col = mColumn->mIndex + 1; col < mColumn->mParentCtrl->getNumColumns(); col++)
        {
            LLScrollListColumn* columnp = mColumn->mParentCtrl->getColumn(col);
            if (!columnp) continue;

            if (columnp->mHeader && columnp->mHeader->canResize())
            {
                // how many pixels in width can this column afford to give up?
                S32 resize_buffer_amt = llmax(0, columnp->getWidth() - MIN_COLUMN_WIDTH);

                // user shrinking column, need to add width to other columns
                if (delta_width < 0)
                {
                    if (columnp->getWidth() > 0)
                    {
                        // statically sized column, give all remaining width to this column
                        columnp->setWidth(columnp->getWidth() + remaining_width);
                        if (columnp->mRelWidth > 0.f)
                        {
                            columnp->mRelWidth = (F32)columnp->getWidth() / (F32)mColumn->mParentCtrl->getItemListRect().getWidth();
                        }
                        // all padding went to this widget, we're done
                        break;
                    }
                }
                else
                {
                    // user growing column, need to take width from other columns
                    remaining_width += resize_buffer_amt;

                    if (columnp->getWidth() > 0)
                    {
                        columnp->setWidth(columnp->getWidth() - llmin(columnp->getWidth() - MIN_COLUMN_WIDTH, delta_width));
                        if (columnp->mRelWidth > 0.f)
                        {
                            columnp->mRelWidth = (F32)columnp->getWidth() / (F32)mColumn->mParentCtrl->getItemListRect().getWidth();
                        }
                    }

                    if (remaining_width >= 0)
                    {
                        // width sucked up from neighboring columns, done
                        break;
                    }
                }
            }
        }

        // clamp resize amount to maximum that can be absorbed by other columns
        if (delta_width > 0)
        {
            delta_width += llmin(remaining_width, 0);
        }

        // propagate constrained delta_width to new width for this column
        new_width = getRect().getWidth() + delta_width - mColumn->mParentCtrl->getColumnPadding();

        // use requested width
        mColumn->setWidth(new_width);

        // update proportional spacing
        if (mColumn->mRelWidth > 0.f)
        {
            mColumn->mRelWidth = (F32)new_width / (F32)mColumn->mParentCtrl->getItemListRect().getWidth();
        }

        // tell scroll list to layout columns again
        // do immediate update to get proper feedback to resize handle
        // which needs to know how far the resize actually went
        const bool force_update = true;
        mColumn->mParentCtrl->updateColumns(force_update);
    }
}

void LLScrollColumnHeader::setHasResizableElement(bool resizable)
{
    if (mHasResizableElement != resizable)
    {
        mColumn->mParentCtrl->dirtyColumns();
        mHasResizableElement = resizable;
    }
}

void LLScrollColumnHeader::updateResizeBars()
{
    S32 num_resizable_columns = 0;
    S32 col;
    for (col = 0; col < mColumn->mParentCtrl->getNumColumns(); col++)
    {
        LLScrollListColumn* columnp = mColumn->mParentCtrl->getColumn(col);
        if (columnp && columnp->mHeader && columnp->mHeader->canResize())
        {
            num_resizable_columns++;
        }
    }

    S32 num_resizers_enabled = 0;

    // now enable/disable resize handles on resizable columns if we have at least two
    for (col = 0; col < mColumn->mParentCtrl->getNumColumns(); col++)
    {
        LLScrollListColumn* columnp = mColumn->mParentCtrl->getColumn(col);
        if (!columnp || !columnp->mHeader) continue;
        bool enable = num_resizable_columns >= 2 && num_resizers_enabled < (num_resizable_columns - 1) && columnp->mHeader->canResize();
        columnp->mHeader->enableResizeBar(enable);
        if (enable)
        {
            num_resizers_enabled++;
        }
    }
}

void LLScrollColumnHeader::enableResizeBar(bool enable)
{
    mResizeBar->setEnabled(enable);
}

bool LLScrollColumnHeader::canResize()
{
    return getVisible() && (mHasResizableElement || mColumn->mDynamicWidth);
}

void LLScrollListColumn::SortNames::declareValues()
{
    declare("ascending", LLScrollListColumn::ASCENDING);
    declare("descending", LLScrollListColumn::DESCENDING);
}

//
// LLScrollListColumn
//
//static
const LLScrollListColumn::Params& LLScrollListColumn::getDefaultParams()
{
    return LLUICtrlFactory::getDefaultParams<LLScrollListColumn>();
}


LLScrollListColumn::LLScrollListColumn(const Params& p, LLScrollListCtrl* parent)
:   mWidth(0),
    mIndex (-1),
    mParentCtrl(parent),
    mName(p.name),
    mLabel(p.header.label),
    mHeader(NULL),
    mMaxContentWidth(0),
    mDynamicWidth(p.width.dynamic_width),
    mRelWidth(p.width.relative_width),
    mFontAlignment(p.halign),
// [SL:KB] - Patch: Control-ScrollList | Checked: Catznip-5.2
    mCanSort(p.can_sort),
// [/SL:KB]
    mSortingColumn(p.sort_column)
{
    if (p.sort_ascending.isProvided())
    {
        mSortDirection = p.sort_ascending() ? ASCENDING : DESCENDING;
    }
    else
    {
        mSortDirection = p.sort_direction;
    }

    setWidth(p.width.pixel_width);
}

void LLScrollListColumn::setWidth(S32 width)
{
    if (!mDynamicWidth && mRelWidth <= 0.f)
    {
        mParentCtrl->updateStaticColumnWidth(this, width);
    }
    mWidth = width;
}
