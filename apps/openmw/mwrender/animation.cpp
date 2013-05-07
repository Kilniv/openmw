#include "animation.hpp"

#include <OgreSkeletonManager.h>
#include <OgreSkeletonInstance.h>
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreParticleSystem.h>
#include <OgreBone.h>
#include <OgreSubMesh.h>
#include <OgreSceneManager.h>

#include "../mwbase/environment.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/character.hpp"

namespace MWRender
{

Animation::AnimLayer::AnimLayer()
  : mSource(NULL)
  , mTime(0.0f)
  , mPlaying(false)
  , mLoopCount(0)
{
}


Ogre::Real Animation::AnimationValue::getValue() const
{
    size_t idx = mIndex;
    while(idx > 0 && mAnimation->mLayer[idx].mGroupName.empty())
        idx--;
    if(!mAnimation->mLayer[idx].mGroupName.empty())
        return mAnimation->mLayer[idx].mTime;
    return 0.0f;
}

void Animation::AnimationValue::setValue(Ogre::Real value)
{
    mAnimation->mLayer[mIndex].mTime = value;
}


void Animation::destroyObjectList(Ogre::SceneManager *sceneMgr, NifOgre::ObjectList &objects)
{
    for(size_t i = 0;i < objects.mParticles.size();i++)
        sceneMgr->destroyParticleSystem(objects.mParticles[i]);
    for(size_t i = 0;i < objects.mEntities.size();i++)
        sceneMgr->destroyEntity(objects.mEntities[i]);
    objects.mControllers.clear();
    objects.mParticles.clear();
    objects.mEntities.clear();
    objects.mSkelBase = NULL;
}

Animation::Animation(const MWWorld::Ptr &ptr)
    : mPtr(ptr)
    , mInsert(NULL)
    , mSkelBase(NULL)
    , mAccumRoot(NULL)
    , mNonAccumRoot(NULL)
    , mNonAccumCtrl(NULL)
    , mAccumulate(0.0f)
    , mLastPosition(0.0f)
    , mAnimVelocity(0.0f)
    , mAnimSpeedMult(1.0f)
{
    for(size_t i = 0;i < sMaxLayers;i++)
        mAnimationValuePtr[i].bind(OGRE_NEW AnimationValue(this, i));
}

Animation::~Animation()
{
    if(mInsert)
    {
        mAnimSources.clear();

        Ogre::SceneManager *sceneMgr = mInsert->getCreator();
        destroyObjectList(sceneMgr, mObjectRoot);
    }
}


void Animation::setObjectRoot(Ogre::SceneNode *node, const std::string &model, bool baseonly)
{
    OgreAssert(!mInsert, "Object already has a root!");
    mInsert = node->createChildSceneNode();

    std::string mdlname = Misc::StringUtils::lowerCase(model);
    std::string::size_type p = mdlname.rfind('\\');
    if(p == std::string::npos)
        p = mdlname.rfind('/');
    if(p != std::string::npos)
        mdlname.insert(mdlname.begin()+p+1, 'x');
    else
        mdlname.insert(mdlname.begin(), 'x');
    if(!Ogre::ResourceGroupManager::getSingleton().resourceExistsInAnyGroup(mdlname))
    {
        mdlname = model;
        Misc::StringUtils::toLower(mdlname);
    }

    mObjectRoot = (!baseonly ? NifOgre::Loader::createObjects(mInsert, mdlname) :
                               NifOgre::Loader::createObjectBase(mInsert, mdlname));
    if(mObjectRoot.mSkelBase)
    {
        mSkelBase = mObjectRoot.mSkelBase;

        Ogre::AnimationStateSet *aset = mObjectRoot.mSkelBase->getAllAnimationStates();
        Ogre::AnimationStateIterator asiter = aset->getAnimationStateIterator();
        while(asiter.hasMoreElements())
        {
            Ogre::AnimationState *state = asiter.getNext();
            state->setEnabled(false);
            state->setLoop(false);
        }

        // Set the bones as manually controlled since we're applying the
        // transformations manually (needed if we want to apply an animation
        // from one skeleton onto another).
        Ogre::SkeletonInstance *skelinst = mObjectRoot.mSkelBase->getSkeleton();
        Ogre::Skeleton::BoneIterator boneiter = skelinst->getBoneIterator();
        while(boneiter.hasMoreElements())
            boneiter.getNext()->setManuallyControlled(true);
    }
    for(size_t i = 0;i < mObjectRoot.mControllers.size();i++)
    {
        if(mObjectRoot.mControllers[i].getSource().isNull())
            mObjectRoot.mControllers[i].setSource(mAnimationValuePtr[0]);
    }
}

void Animation::setRenderProperties(const NifOgre::ObjectList &objlist, Ogre::uint32 visflags, Ogre::uint8 solidqueue, Ogre::uint8 transqueue)
{
    for(size_t i = 0;i < objlist.mEntities.size();i++)
    {
        Ogre::Entity *ent = objlist.mEntities[i];
        if(visflags != 0)
            ent->setVisibilityFlags(visflags);

        for(unsigned int j = 0;j < ent->getNumSubEntities();++j)
        {
            Ogre::SubEntity* subEnt = ent->getSubEntity(j);
            subEnt->setRenderQueueGroup(subEnt->getMaterial()->isTransparent() ? transqueue : solidqueue);
        }
    }
    for(size_t i = 0;i < objlist.mParticles.size();i++)
    {
        Ogre::ParticleSystem *part = objlist.mParticles[i];
        if(visflags != 0)
            part->setVisibilityFlags(visflags);
        // TODO: Check particle material for actual transparency
        part->setRenderQueueGroup(transqueue);
    }
}


void Animation::addAnimSource(const std::string &model)
{
    OgreAssert(mInsert, "Object is missing a root!");
    if(!mSkelBase)
        return;

    std::string kfname = Misc::StringUtils::lowerCase(model);
    std::string::size_type p = kfname.rfind('\\');
    if(p == std::string::npos)
        p = kfname.rfind('/');
    if(p != std::string::npos)
        kfname.insert(kfname.begin()+p+1, 'x');
    else
        kfname.insert(kfname.begin(), 'x');

    if(kfname.size() > 4 && kfname.compare(kfname.size()-4, 4, ".nif") == 0)
        kfname.replace(kfname.size()-4, 4, ".kf");

    if(!Ogre::ResourceGroupManager::getSingleton().resourceExistsInAnyGroup(kfname))
        return;

    mAnimSources.push_back(AnimSource());
    NifOgre::Loader::createKfControllers(mSkelBase, kfname,
                                         mAnimSources.back().mTextKeys,
                                         mAnimSources.back().mControllers);
    if(mAnimSources.back().mTextKeys.size() == 0 || mAnimSources.back().mControllers.size() == 0)
    {
        mAnimSources.pop_back();
        return;
    }

    std::vector<Ogre::Controller<Ogre::Real> > &ctrls = mAnimSources.back().mControllers;
    NifOgre::NodeTargetValue<Ogre::Real> *dstval;

    for(size_t i = 0;i < ctrls.size();i++)
    {
        dstval = static_cast<NifOgre::NodeTargetValue<Ogre::Real>*>(ctrls[i].getDestination().getPointer());

        if(i == 0 && !mAccumRoot)
        {
            mAccumRoot = mInsert;
            mNonAccumRoot = dstval->getNode();
        }

        ctrls[i].setSource(mAnimationValuePtr[0]);
    }
}

void Animation::clearAnimSources()
{
    for(size_t layer = 0;layer < sMaxLayers;layer++)
    {
        mLayer[layer].mGroupName.clear();
        mLayer[layer].mSource = NULL;
        mLayer[layer].mTime = 0.0f;
        mLayer[layer].mLoopCount = 0;
        mLayer[layer].mPlaying = false;
    }
    mNonAccumCtrl = NULL;
    mAnimVelocity = 0.0f;

    mLastPosition = Ogre::Vector3(0.0f);
    mAccumRoot = NULL;
    mNonAccumRoot = NULL;

    mAnimSources.clear();
}


Ogre::Node *Animation::getNode(const std::string &name)
{
    if(mSkelBase)
    {
        Ogre::SkeletonInstance *skel = mSkelBase->getSkeleton();
        if(skel->hasBone(name))
            return skel->getBone(name);
    }
    return NULL;
}


NifOgre::TextKeyMap::const_iterator Animation::findGroupStart(const NifOgre::TextKeyMap &keys, const std::string &groupname)
{
    NifOgre::TextKeyMap::const_iterator iter(keys.begin());
    for(;iter != keys.end();iter++)
    {
        if(iter->second.compare(0, groupname.size(), groupname) == 0 &&
           iter->second.compare(groupname.size(), 2, ": ") == 0)
            break;
    }
    return iter;
}


bool Animation::hasAnimation(const std::string &anim)
{
    AnimSourceList::const_iterator iter(mAnimSources.begin());
    for(;iter != mAnimSources.end();iter++)
    {
        const NifOgre::TextKeyMap &keys = iter->mTextKeys;
        if(findGroupStart(keys, anim) != keys.end())
            return true;
    }

    return false;
}


void Animation::setAccumulation(const Ogre::Vector3 &accum)
{
    mAccumulate = accum;
}

void Animation::setSpeed(float speed)
{
    mAnimSpeedMult = 1.0f;
    if(mAnimVelocity > 1.0f && speed > 0.0f)
        mAnimSpeedMult = speed / mAnimVelocity;
}


void Animation::updatePtr(const MWWorld::Ptr &ptr)
{
    mPtr = ptr;
}


float Animation::calcAnimVelocity(const NifOgre::TextKeyMap &keys, NifOgre::NodeTargetValue<Ogre::Real> *nonaccumctrl, const Ogre::Vector3 &accum, const std::string &groupname)
{
    const std::string start = groupname+": start";
    const std::string loopstart = groupname+": loop start";
    const std::string loopstop = groupname+": loop stop";
    const std::string stop = groupname+": stop";
    float starttime = std::numeric_limits<float>::max();
    float stoptime = 0.0f;
    NifOgre::TextKeyMap::const_iterator keyiter(keys.begin());
    while(keyiter != keys.end())
    {
        if(keyiter->second == start || keyiter->second == loopstart)
            starttime = keyiter->first;
        else if(keyiter->second == loopstop || keyiter->second == stop)
        {
            stoptime = keyiter->first;
            break;
        }
        keyiter++;
    }

    if(stoptime > starttime)
    {
        Ogre::Vector3 startpos = nonaccumctrl->getTranslation(starttime) * accum;
        Ogre::Vector3 endpos = nonaccumctrl->getTranslation(stoptime) * accum;

        return startpos.distance(endpos) / (stoptime-starttime);
    }

    return 0.0f;
}

static void updateBoneTree(const Ogre::SkeletonInstance *skelsrc, Ogre::Bone *bone)
{
    if(skelsrc->hasBone(bone->getName()))
    {
        Ogre::Bone *srcbone = skelsrc->getBone(bone->getName());
        if(!srcbone->getParent() || !bone->getParent())
        {
            bone->setOrientation(srcbone->getOrientation());
            bone->setPosition(srcbone->getPosition());
            bone->setScale(srcbone->getScale());
        }
        else
        {
            bone->_setDerivedOrientation(srcbone->_getDerivedOrientation());
            bone->_setDerivedPosition(srcbone->_getDerivedPosition());
            bone->setScale(Ogre::Vector3::UNIT_SCALE);
        }
    }
    else
    {
        // No matching bone in the source. Make sure it stays properly offset
        // from its parent.
        bone->resetToInitialState();
    }

    Ogre::Node::ChildNodeIterator boneiter = bone->getChildIterator();
    while(boneiter.hasMoreElements())
        updateBoneTree(skelsrc, static_cast<Ogre::Bone*>(boneiter.getNext()));
}

void Animation::updateSkeletonInstance(const Ogre::SkeletonInstance *skelsrc, Ogre::SkeletonInstance *skel)
{
    Ogre::Skeleton::BoneIterator boneiter = skel->getRootBoneIterator();
    while(boneiter.hasMoreElements())
        updateBoneTree(skelsrc, boneiter.getNext());
}


void Animation::updatePosition(Ogre::Vector3 &position)
{
    Ogre::Vector3 posdiff;

    /* Get the non-accumulation root's difference from the last update, and move the position
     * accordingly.
     */
    posdiff = (mNonAccumCtrl->getTranslation(mLayer[0].mTime) - mLastPosition) * mAccumulate;
    position += posdiff;

    /* Translate the accumulation root back to compensate for the move. */
    mLastPosition += posdiff;
    mAccumRoot->setPosition(-mLastPosition);
}

bool Animation::reset(size_t layeridx, const NifOgre::TextKeyMap &keys, NifOgre::NodeTargetValue<Ogre::Real> *nonaccumctrl, const std::string &groupname, const std::string &start, const std::string &stop, float startpoint)
{
    std::string tag = groupname+": "+start;
    NifOgre::TextKeyMap::const_iterator startkey(keys.begin());
    while(startkey != keys.end() && startkey->second != tag)
        startkey++;
    if(startkey == keys.end() && start == "loop start")
    {
        tag = groupname+": start";
        startkey = keys.begin();
        while(startkey != keys.end() && startkey->second != tag)
            startkey++;
    }
    if(startkey == keys.end())
        return false;

    tag = groupname+": "+stop;
    NifOgre::TextKeyMap::const_iterator stopkey(startkey);
    while(stopkey != keys.end() && stopkey->second != tag)
        stopkey++;
    if(stopkey == keys.end())
        return false;

    if(startkey == stopkey)
        return false;

    mLayer[layeridx].mStartKey = startkey;
    mLayer[layeridx].mLoopStartKey = startkey;
    mLayer[layeridx].mStopKey = stopkey;
    mLayer[layeridx].mNextKey = startkey;

    mLayer[layeridx].mTime = mLayer[layeridx].mStartKey->first + ((mLayer[layeridx].mStopKey->first-
                                                                   mLayer[layeridx].mStartKey->first) * startpoint);

    tag = groupname+": loop start";
    while(mLayer[layeridx].mNextKey->first <= mLayer[layeridx].mTime && mLayer[layeridx].mNextKey != mLayer[layeridx].mStopKey)
    {
        if(mLayer[layeridx].mNextKey->second == tag)
            mLayer[layeridx].mLoopStartKey = mLayer[layeridx].mNextKey;
        mLayer[layeridx].mNextKey++;
    }

    if(layeridx == 0 && nonaccumctrl)
        mLastPosition = nonaccumctrl->getTranslation(mLayer[layeridx].mTime) * mAccumulate;

    return true;
}

bool Animation::doLoop(size_t layeridx)
{
    if(mLayer[layeridx].mLoopCount == 0)
        return false;
    mLayer[layeridx].mLoopCount--;

    mLayer[layeridx].mTime = mLayer[layeridx].mLoopStartKey->first;
    mLayer[layeridx].mNextKey = mLayer[layeridx].mLoopStartKey;
    mLayer[layeridx].mNextKey++;
    mLayer[layeridx].mPlaying = true;
    if(layeridx == 0 && mNonAccumCtrl)
        mLastPosition = mNonAccumCtrl->getTranslation(mLayer[layeridx].mTime) * mAccumulate;

    return true;
}


bool Animation::handleTextKey(size_t layeridx, const NifOgre::TextKeyMap::const_iterator &key)
{
    float time = key->first;
    const std::string &evt = key->second;

    if(evt.compare(0, 7, "sound: ") == 0)
    {
        MWBase::SoundManager *sndMgr = MWBase::Environment::get().getSoundManager();
        sndMgr->playSound3D(mPtr, evt.substr(7), 1.0f, 1.0f);
        return true;
    }
    if(evt.compare(0, 10, "soundgen: ") == 0)
    {
        // FIXME: Lookup the SoundGen (SNDG) for the specified sound that corresponds
        // to this actor type
        return true;
    }

    if(evt.compare(0, mLayer[layeridx].mGroupName.size(), mLayer[layeridx].mGroupName) != 0 ||
       evt.compare(mLayer[layeridx].mGroupName.size(), 2, ": ") != 0)
    {
        // Not ours, skip it
        return true;
    }
    size_t off = mLayer[layeridx].mGroupName.size()+2;
    size_t len = evt.size() - off;

    if(evt.compare(off, len, "start") == 0 || evt.compare(off, len, "loop start") == 0)
    {
        mLayer[layeridx].mLoopStartKey = key;
        return true;
    }

    if(evt.compare(off, len, "loop stop") == 0 || evt.compare(off, len, "stop") == 0)
    {
        if(doLoop(layeridx))
        {
            if(mLayer[layeridx].mTime >= time)
                return false;
        }
        return true;
    }

    std::cerr<< "Unhandled animation textkey: "<<evt <<std::endl;
    return true;
}


bool Animation::play(const std::string &groupname, const std::string &start, const std::string &stop, float startpoint, size_t loops)
{
    // TODO: parameterize this
    size_t layeridx = 0;

    if(!mSkelBase)
        return false;

    mLayer[layeridx].mGroupName.clear();
    mLayer[layeridx].mSource = NULL;
    mLayer[layeridx].mTime = 0.0f;
    mLayer[layeridx].mLoopCount = 0;
    mLayer[layeridx].mPlaying = false;

    if(groupname.empty())
        return false;

    bool movinganim = false;
    bool foundanim = false;

    /* Look in reverse; last-inserted source has priority. */
    AnimSourceList::reverse_iterator iter(mAnimSources.rbegin());
    for(;iter != mAnimSources.rend();iter++)
    {
        const NifOgre::TextKeyMap &keys = iter->mTextKeys;
        NifOgre::NodeTargetValue<Ogre::Real> *nonaccumctrl = NULL;
        if(layeridx == 0 && mNonAccumRoot)
        {
            for(size_t i = 0;i < iter->mControllers.size();i++)
            {
                NifOgre::NodeTargetValue<Ogre::Real> *dstval;
                dstval = dynamic_cast<NifOgre::NodeTargetValue<Ogre::Real>*>(iter->mControllers[i].getDestination().getPointer());
                if(dstval && dstval->getNode() == mNonAccumRoot)
                {
                    nonaccumctrl = dstval;
                    break;
                }
            }
        }

        if(!foundanim)
        {
            if(!reset(layeridx, keys, nonaccumctrl, groupname, start, stop, startpoint))
                continue;

            mLayer[layeridx].mGroupName = groupname;
            mLayer[layeridx].mSource = &*iter;
            mLayer[layeridx].mLoopCount = loops;
            mLayer[layeridx].mPlaying = true;

            if(layeridx == 0)
            {
                mNonAccumCtrl = nonaccumctrl;
                mAnimVelocity = 0.0f;
            }

            foundanim = true;

            if(mAccumulate == Ogre::Vector3(0.0f))
                break;
        }

        if(!nonaccumctrl)
            break;

        mAnimVelocity = calcAnimVelocity(keys, nonaccumctrl, mAccumulate, groupname);
        if(mAnimVelocity > 1.0f)
        {
            movinganim = (nonaccumctrl==mNonAccumCtrl);
            break;
        }
    }
    if(!foundanim)
        std::cerr<< "Failed to find animation "<<groupname <<std::endl;

    return movinganim;
}

void Animation::disable(size_t layeridx)
{
    if(mLayer[layeridx].mGroupName.empty())
        return;

    mLayer[layeridx].mGroupName.clear();
    mLayer[layeridx].mSource = NULL;
    mLayer[layeridx].mTime = 0.0f;
    mLayer[layeridx].mLoopCount = 0;
    mLayer[layeridx].mPlaying = false;
}

bool Animation::getInfo(size_t layeridx, float *complete, std::string *groupname, std::string *start, std::string *stop) const
{
    if(mLayer[layeridx].mGroupName.empty())
    {
        if(complete) *complete = 0.0f;
        if(groupname) *groupname = "";
        if(start) *start = "";
        if(stop) *stop = "";
        return false;
    }

    if(complete) *complete = (mLayer[layeridx].mTime - mLayer[layeridx].mStartKey->first) /
                             (mLayer[layeridx].mStopKey->first - mLayer[layeridx].mStartKey->first);
    if(groupname) *groupname = mLayer[layeridx].mGroupName;
    if(start) *start = mLayer[layeridx].mStartKey->second.substr(mLayer[layeridx].mGroupName.size()+2);
    if(stop) *stop = mLayer[layeridx].mStopKey->second.substr(mLayer[layeridx].mGroupName.size()+2);
    return true;
}


Ogre::Vector3 Animation::runAnimation(float duration)
{
    Ogre::Vector3 movement(0.0f);

    duration *= mAnimSpeedMult;
    for(size_t layeridx = 0;layeridx < sMaxLayers;layeridx++)
    {
        if(mLayer[layeridx].mGroupName.empty())
            continue;

        float timepassed = duration;
        while(mLayer[layeridx].mPlaying)
        {
            float targetTime = mLayer[layeridx].mTime + timepassed;
            if(mLayer[layeridx].mNextKey->first > targetTime)
            {
                mLayer[layeridx].mTime = targetTime;
                if(layeridx == 0 && mNonAccumCtrl)
                    updatePosition(movement);
                break;
            }

            NifOgre::TextKeyMap::const_iterator key(mLayer[layeridx].mNextKey++);
            mLayer[layeridx].mTime = key->first;
            if(layeridx == 0 && mNonAccumCtrl)
                updatePosition(movement);

            mLayer[layeridx].mPlaying = (key != mLayer[layeridx].mStopKey);
            timepassed = targetTime - mLayer[layeridx].mTime;

            if(!handleTextKey(layeridx, key))
                break;
        }
    }

    for(size_t i = 0;i < mObjectRoot.mControllers.size();i++)
        mObjectRoot.mControllers[i].update();
    for(size_t layeridx = 0;layeridx < sMaxLayers;layeridx++)
    {
        if(mLayer[layeridx].mGroupName.empty())
            continue;

        for(size_t i = 0;i < mLayer[layeridx].mSource->mControllers.size();i++)
            mLayer[layeridx].mSource->mControllers[i].update();
    }

    if(mSkelBase)
    {
        // HACK: Dirty the animation state set so that Ogre will apply the
        // transformations to entities this skeleton instance is shared with.
        mSkelBase->getAllAnimationStates()->_notifyDirty();
    }

    return movement;
}

void Animation::showWeapons(bool showWeapon)
{
}

}
