#include "enchanting.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwbase/mechanicsmanager.hpp"

#include "creaturestats.hpp"
#include "npcstats.hpp"
#include "spellcasting.hpp"

namespace MWMechanics
{
    Enchanting::Enchanting()
        : mCastStyle(ESM::Enchantment::CastOnce)
        , mSelfEnchanting(false)
    {}

    void Enchanting::setOldItem(MWWorld::Ptr oldItem)
    {
        mOldItemPtr=oldItem;
        if(!itemEmpty())
        {
            mObjectType = mOldItemPtr.getTypeName();
        }
        else
        {
            mObjectType="";
        }
    }

    void Enchanting::setNewItemName(const std::string& s)
    {
        mNewItemName=s;
    }

    void Enchanting::setEffect(ESM::EffectList effectList)
    {
        mEffectList=effectList;
    }

    int Enchanting::getCastStyle() const
    {
        return mCastStyle;
    }

    void Enchanting::setSoulGem(MWWorld::Ptr soulGem)
    {
        mSoulGemPtr=soulGem;
    }

    bool Enchanting::create()
    {
        const MWWorld::Ptr& player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        MWWorld::ContainerStore& store = player.getClass().getContainerStore(player);
        ESM::Enchantment enchantment;
        enchantment.mData.mCharge = getGemCharge();
        enchantment.mData.mAutocalc = 0;
        enchantment.mData.mType = mCastStyle;
        enchantment.mData.mCost = getBaseCastCost();

        store.remove(mSoulGemPtr, 1, player);

        //Exception for Azura Star, new one will be added after enchanting
        if(Misc::StringUtils::ciEqual(mSoulGemPtr.get<ESM::Miscellaneous>()->mBase->mId, "Misc_SoulGem_Azura"))
            store.add("Misc_SoulGem_Azura", 1, player);

        if(mSelfEnchanting)
        {
            if(getEnchantChance()<std::rand()/static_cast<double> (RAND_MAX)*100)
                return false;

            mEnchanter.getClass().skillUsageSucceeded (mEnchanter, ESM::Skill::Enchant, 2);
        }

        if(mCastStyle==ESM::Enchantment::ConstantEffect)
        {
            enchantment.mData.mCharge=0;
        }
        enchantment.mEffects = mEffectList;

        // Apply the enchantment
        const ESM::Enchantment *enchantmentPtr = MWBase::Environment::get().getWorld()->createRecord (enchantment);
        std::string newItemId = mOldItemPtr.getClass().applyEnchantment(mOldItemPtr, enchantmentPtr->mId, getGemCharge(), mNewItemName);

        // Add the new item to player inventory and remove the old one
        store.remove(mOldItemPtr, 1, player);
        store.add(newItemId, 1, player);

        if(!mSelfEnchanting)
            payForEnchantment();

        return true;
    }
    
    void Enchanting::nextCastStyle()
    {
        if (itemEmpty())
        {
            mCastStyle = ESM::Enchantment::WhenUsed;
            return;
        }

        const bool powerfulSoul = getGemCharge() >= \
                MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find ("iSoulAmountForConstantEffect")->getInt();
        if ((mObjectType == typeid(ESM::Armor).name()) || (mObjectType == typeid(ESM::Clothing).name()))
        { // Armor or Clothing
            switch(mCastStyle)
            {
                case ESM::Enchantment::WhenUsed:
                    if (powerfulSoul)
                        mCastStyle = ESM::Enchantment::ConstantEffect;
                    return;
                default: // takes care of Constant effect too
                    mCastStyle = ESM::Enchantment::WhenUsed;
                    return;
            }
        }
        else if(mObjectType == typeid(ESM::Weapon).name())
        { // Weapon
            switch(mCastStyle)
            {
                case ESM::Enchantment::WhenStrikes:
                    mCastStyle = ESM::Enchantment::WhenUsed;
                    return;
                case ESM::Enchantment::WhenUsed:
                    if (powerfulSoul)
                        mCastStyle = ESM::Enchantment::ConstantEffect;
                    else
                        mCastStyle = ESM::Enchantment::WhenStrikes;
                    return;
                default: // takes care of Constant effect too
                    mCastStyle = ESM::Enchantment::WhenStrikes;
                    return;
            }
        }
        else if(mObjectType == typeid(ESM::Book).name())
        { // Scroll or Book
            mCastStyle = ESM::Enchantment::CastOnce;
            return;
        }

        // Fail case
        mCastStyle = ESM::Enchantment::CastOnce;
    }

    /*
     * Vanilla enchant cost formula:
     *
     *  Touch/Self:          (min + max) * baseCost * 0.025 * duration + area * baseCost * 0.025
     *  Target:       1.5 * ((min + max) * baseCost * 0.025 * duration + area * baseCost * 0.025)
     *  Constant eff:        (min + max) * baseCost * 2.5              + area * baseCost * 0.025
     *
     *  For multiple effects - cost of each effect is multiplied by number of effects that follows +1.
     *
     *  Note: Minimal value inside formula for 'min' and 'max' is 1. So in vanilla:
     *        (0 + 0) == (1 + 0) == (1 + 1) => 2 or (2 + 0) == (1 + 2) => 3
     *
     *  Formula on UESPWiki is not entirely correct.
     */
    int Enchanting::getEnchantPoints() const
    {
        if (mEffectList.mList.empty())
            // No effects added, cost = 0
            return 0;

        const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();
        std::vector<ESM::ENAMstruct> mEffects = mEffectList.mList;

        float enchantmentCost = 0;
        int effectsLeftCnt = mEffects.size();
        for (std::vector<ESM::ENAMstruct>::const_iterator it = mEffects.begin(); it != mEffects.end(); ++it)
        {
            float baseCost = (store.get<ESM::MagicEffect>().find(it->mEffectID))->mData.mBaseCost;
            // To reflect vanilla behavior
            int magMin = (it->mMagnMin == 0) ? 1 : it->mMagnMin;
            int magMax = (it->mMagnMax == 0) ? 1 : it->mMagnMax;
            int area = (it->mArea == 0) ? 1 : it->mArea;

            float magnitudeCost = 0;
            if (mCastStyle == ESM::Enchantment::ConstantEffect)
            {
                magnitudeCost = (magMin + magMax) * baseCost * 2.5;
            }
            else
            {
                magnitudeCost = (magMin + magMax) * it->mDuration * baseCost * 0.025;
                if(it->mRange == ESM::RT_Target)
                    magnitudeCost *= 1.5;
            }

            float areaCost = area * 0.025 * baseCost;
            if (it->mRange == ESM::RT_Target)
                areaCost *= 1.5;

            enchantmentCost += (magnitudeCost + areaCost) * effectsLeftCnt;
            --effectsLeftCnt;
        }

        return static_cast<int>(enchantmentCost);
    }


    int Enchanting::getBaseCastCost() const
    {
        if (mCastStyle == ESM::Enchantment::ConstantEffect)
            return 0;

        return getEnchantPoints();
    }

    int Enchanting::getEffectiveCastCost() const
    {
        int baseCost = getBaseCastCost();
        MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        return getEffectiveEnchantmentCastCost(baseCost, player);
    }


    int Enchanting::getEnchantPrice() const
    {
        if(mEnchanter.isEmpty())
            return 0;

        float priceMultipler = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>().find ("fEnchantmentValueMult")->getFloat();
        int price = MWBase::Environment::get().getMechanicsManager()->getBarterOffer(mEnchanter, (getEnchantPoints() * priceMultipler), true);
        return price;
    }

    int Enchanting::getGemCharge() const
    {
        const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();
        if(soulEmpty())
            return 0;
        if(mSoulGemPtr.getCellRef().getSoul()=="")
            return 0;
        const ESM::Creature* soul = store.get<ESM::Creature>().find(mSoulGemPtr.getCellRef().getSoul());
        return soul->mData.mSoul;
    }

    int Enchanting::getMaxEnchantValue() const
    {
        if (itemEmpty())
            return 0;

        const MWWorld::ESMStore &store = MWBase::Environment::get().getWorld()->getStore();

        return mOldItemPtr.getClass().getEnchantmentPoints(mOldItemPtr) * store.get<ESM::GameSetting>().find("fEnchantmentMult")->getFloat();
    }
    bool Enchanting::soulEmpty() const
    {
        return mSoulGemPtr.isEmpty();
    }

    bool Enchanting::itemEmpty() const
    {
        return mOldItemPtr.isEmpty();
    }

    void Enchanting::setSelfEnchanting(bool selfEnchanting)
    {
        mSelfEnchanting = selfEnchanting;
    }

    void Enchanting::setEnchanter(MWWorld::Ptr enchanter)
    {
        mEnchanter = enchanter;
    }

    float Enchanting::getEnchantChance() const
    {
        const NpcStats& npcStats = mEnchanter.getClass().getNpcStats (mEnchanter);

        float chance1 = (npcStats.getSkill (ESM::Skill::Enchant).getModified() + 
        (0.25 * npcStats.getAttribute (ESM::Attribute::Intelligence).getModified())
        + (0.125 * npcStats.getAttribute (ESM::Attribute::Luck).getModified()));

        const MWWorld::Store<ESM::GameSetting>& gmst = MWBase::Environment::get().getWorld()->getStore().get<ESM::GameSetting>();

        float chance2 = 7.5 / (gmst.find("fEnchantmentChanceMult")->getFloat() * ((mCastStyle == ESM::Enchantment::ConstantEffect) ?
                                                                          gmst.find("fEnchantmentConstantChanceMult")->getFloat() : 1 ))
                * getEnchantPoints();

        return (chance1-chance2);
    }

    void Enchanting::payForEnchantment() const
    {
        const MWWorld::Ptr& player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        MWWorld::ContainerStore& store = player.getClass().getContainerStore(player);

        store.remove(MWWorld::ContainerStore::sGoldId, getEnchantPrice(), player);

        // add gold to NPC trading gold pool
        CreatureStats& enchanterStats = mEnchanter.getClass().getCreatureStats(mEnchanter);
        enchanterStats.setGoldPool(enchanterStats.getGoldPool() + getEnchantPrice());
    }
}
