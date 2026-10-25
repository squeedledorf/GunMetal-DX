/**
 * @file llfloatertoolstexturedock.h
 * @brief #577: companion floater that hosts the Build window's Texture panel
 *        beside the Build window while faces are picked in the Links tab.
 *
 * The floater owns no panel of its own. LLFloaterTools reparents its single
 * live Texture panel (FSPanelFace / LLPanelFace) into it and back again.
 */
#ifndef LL_LLFLOATERTOOLSTEXTUREDOCK_H
#define LL_LLFLOATERTOOLSTEXTUREDOCK_H

#include "llfloater.h"

class LLPanel;

class LLFloaterToolsTextureDock final : public LLFloater
{
    friend class LLFloaterReg;
public:
    ~LLFloaterToolsTextureDock() override;

    bool postBuild() override;
    void onClose(bool app_quitting) override;
    void draw() override;

    // Take a panel (already removed from its previous parent) and size to fit.
    void attachPanel(LLPanel* panel);
    // #969: take two panels side by side (Object | Contents) and size to fit.
    void attachPanels(LLPanel* left, LLPanel* right);
    // Drop the panel from this floater's children; the caller re-homes it.
    void detachPanel(LLPanel* panel);

private:
    LLFloaterToolsTextureDock(const LLSD& key);
    void followTools();

    LLPanel* mPanel = nullptr;
    std::string mTextureTitle;
    LLPanel* mPanel2 = nullptr;     // #969: second (right-hand) panel in link mode
};

#endif
