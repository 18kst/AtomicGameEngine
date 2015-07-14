//
// Copyright (c) 2008-2014 the Urho3D project.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//

// Copyright (c) 2014-2015, THUNDERBEAST GAMES LLC All rights reserved
// Please see LICENSE.md in repository root for license information
// https://github.com/AtomicGameEngine/AtomicGameEngine

#include <Atomic/IO/Log.h>
#include <Atomic/IO/FileSystem.h>
#include <Atomic/Core/Context.h>
#include <Atomic/Resource/ResourceCache.h>

#ifdef ATOMIC_PHYSICS
#include <Atomic/Physics/PhysicsEvents.h>
#include <Atomic/Physics/PhysicsWorld.h>
#endif
#include <Atomic/Scene/Scene.h>
#include <Atomic/Scene/SceneEvents.h>

#include "JSVM.h"
#include "JSComponentFile.h"
#include "JSComponent.h"

namespace Atomic
{

extern const char* LOGIC_CATEGORY;

class JSComponentFactory : public ObjectFactory
{
public:
    /// Construct.
    JSComponentFactory(Context* context) :
        ObjectFactory(context)
    {
        type_ = JSComponent::GetTypeStatic();
        baseType_ = JSComponent::GetBaseTypeStatic();
        typeName_ = JSComponent::GetTypeNameStatic();
    }

    /// Create an object of the specific type.
    SharedPtr<Object> CreateObject(const XMLElement& source = XMLElement::EMPTY)
    {

        // if in editor, just create the JSComponent
        if (context_->GetEditorContext())
        {
            return SharedPtr<Object>(new JSComponent(context_));
        }

        // At runtime, a XML JSComponent may refer to a "scriptClass"
        // component which is new'd in JS and creates the component itself
        // we peek ahead here to see if we have a JSComponentFile and if it is a script class

        String componentRef;

        if (source != XMLElement::EMPTY)
        {
            XMLElement attrElem = source.GetChild("attribute");

            while (attrElem)
            {
                if (attrElem.GetAttribute("name") == "ComponentFile")
                {
                    componentRef = attrElem.GetAttribute("value");
                    break;
                }

                attrElem = attrElem.GetNext("attribute");
            }
        }

        SharedPtr<Object> ptr;

        if (componentRef.Length())
        {
            Vector<String> split = componentRef.Split(';');

            if (split.Size() == 2)
            {
                ResourceCache* cache = context_->GetSubsystem<ResourceCache>();
                JSComponentFile* componentFile = cache->GetResource<JSComponentFile>(split[1]);
                ptr = componentFile->CreateJSComponent();
            }

        }

        if (ptr.Null())
        {
            ptr = new JSComponent(context_);
        }

        return ptr;

    }
};


JSComponent::JSComponent(Context* context) :
    Component(context),
    updateEventMask_(USE_UPDATE | USE_POSTUPDATE | USE_FIXEDUPDATE | USE_FIXEDPOSTUPDATE),
    currentEventMask_(0),
    started_(false),
    destroyed_(false),
    scriptClassInstance_(false),
    delayedStartCalled_(false),
    loading_(false)
{
    vm_ = JSVM::GetJSVM(NULL);
}

JSComponent::~JSComponent()
{

}

void JSComponent::RegisterObject(Context* context)
{
    context->RegisterFactory(new JSComponentFactory(context), LOGIC_CATEGORY);


    ACCESSOR_ATTRIBUTE("Is Enabled", IsEnabled, SetEnabled, bool, true, AM_DEFAULT);
    ATTRIBUTE("FieldValues", VariantMap, fieldValues_, Variant::emptyVariantMap, AM_FILE);
    MIXED_ACCESSOR_ATTRIBUTE("ComponentFile", GetScriptAttr, SetScriptAttr, ResourceRef, ResourceRef(JSComponentFile::GetTypeStatic()), AM_DEFAULT);
}

void JSComponent::OnSetEnabled()
{
    UpdateEventSubscription();
}

void JSComponent::SetUpdateEventMask(unsigned char mask)
{
    if (updateEventMask_ != mask)
    {
        updateEventMask_ = mask;
        UpdateEventSubscription();
    }
}

void JSComponent::UpdateReferences(bool remove)
{
    duk_context* ctx = vm_->GetJSContext();

    int top = duk_get_top(ctx);

    duk_push_global_stash(ctx);
    duk_get_prop_index(ctx, -1, JS_GLOBALSTASH_INDEX_NODE_REGISTRY);

    // can't use instance as key, as this coerces to [Object] for
    // string property, pointer will be string representation of
    // address, so, unique key

    if (node_)
    {
        duk_push_pointer(ctx, (void*) node_);
        if (remove)
            duk_push_undefined(ctx);
        else
            js_push_class_object_instance(ctx, node_);

        duk_put_prop(ctx, -3);
    }

    duk_push_pointer(ctx, (void*) this);
    if (remove)
        duk_push_undefined(ctx);
    else
        js_push_class_object_instance(ctx, this);

    duk_put_prop(ctx, -3);

    duk_pop_2(ctx);

    assert(duk_get_top(ctx) == top);
}

void JSComponent::ApplyAttributes()
{
    if (!started_)
        InitInstance();
}

void JSComponent::InitInstance(bool hasArgs, int argIdx)
{
    if (context_->GetEditorContext() || componentFile_.Null())
        return;

    duk_context* ctx = vm_->GetJSContext();

    duk_idx_t top = duk_get_top(ctx);

    // store, so pop doesn't clear
    UpdateReferences();

    // apply fields

    const HashMap<String, VariantType>& fields =  componentFile_->GetFields();

    if (fields.Size())
    {
        // push self
        js_push_class_object_instance(ctx, this, "JSComponent");

        HashMap<String, VariantType>::ConstIterator itr = fields.Begin();
        while (itr != fields.End())
        {
            if (fieldValues_.Contains(itr->first_))
            {
                Variant& v = fieldValues_[itr->first_];

                if (v.GetType() == itr->second_)
                {
                    js_push_variant(ctx, v);
                    duk_put_prop_string(ctx, -2, itr->first_.CString());
                }
            }
            else
            {
                Variant v;
                componentFile_->GetDefaultFieldValue(itr->first_, v);
                js_push_variant(ctx,  v);
                duk_put_prop_string(ctx, -2, itr->first_.CString());
            }

            itr++;
        }

        // pop self
        duk_pop(ctx);
    }

    // apply args if any
    if (hasArgs)
    {
        // push self
        js_push_class_object_instance(ctx, this, "JSComponent");

        duk_enum(ctx, argIdx, DUK_ENUM_OWN_PROPERTIES_ONLY);

        while (duk_next(ctx, -1, 1)) {

            duk_put_prop(ctx, -4);

        }

        // pop self and enum object
        duk_pop_2(ctx);

    }

    if (!componentFile_->GetScriptClass())
    {

        componentFile_->PushModule();

        if (!duk_is_function(ctx, -1))
        {
            duk_set_top(ctx, top);
            return;
        }

        // call with self
        js_push_class_object_instance(ctx, this, "JSComponent");

        if (duk_pcall(ctx, 1) != 0)
        {
            vm_->SendJSErrorEvent();
            duk_set_top(ctx, top);
            return;
        }

    }

    duk_set_top(ctx, top);

    if (!started_)
    {
        started_ = true;
        Start();
    }

}

void JSComponent::CallScriptMethod(const String& name, bool passValue, float value)
{
    void* heapptr = JSGetHeapPtr();

    if (!heapptr)
        return;

    duk_context* ctx = vm_->GetJSContext();

    duk_idx_t top = duk_get_top(ctx);

    duk_push_heapptr(ctx, heapptr);

    duk_get_prop_string(ctx, -1, name.CString());

    if (!duk_is_function(ctx, -1))
    {
        duk_set_top(ctx, top);
        return;
    }

    // push this
    if (scriptClassInstance_)
        duk_push_heapptr(ctx, heapptr);

    if (passValue)
        duk_push_number(ctx, value);

    int status = scriptClassInstance_ ? duk_pcall_method(ctx, passValue ? 1 : 0) : duk_pcall(ctx, passValue ? 1 : 0);

    if (status != 0)
    {
        vm_->SendJSErrorEvent();
        duk_set_top(ctx, top);
        return;
    }

    duk_set_top(ctx, top);
}

void JSComponent::Start()
{
    static String name = "start";
    CallScriptMethod(name);
}

void JSComponent::DelayedStart()
{
    static String name = "delayedStart";
    CallScriptMethod(name);
}

void JSComponent::Update(float timeStep)
{
    static String name = "update";
    CallScriptMethod(name, true, timeStep);
}

void JSComponent::PostUpdate(float timeStep)
{
    static String name = "postUpdate";
    CallScriptMethod(name, true, timeStep);
}

void JSComponent::FixedUpdate(float timeStep)
{
    static String name = "fixedUpdate";
    CallScriptMethod(name, true, timeStep);
}

void JSComponent::FixedPostUpdate(float timeStep)
{
    static String name = "fixedPostUpdate";
    CallScriptMethod(name, true, timeStep);
}

void JSComponent::OnNodeSet(Node* node)
{
    if (node)
    {

    }
    else
    {
        // We are being detached from a node: execute user-defined stop function and prepare for destruction
        UpdateReferences(true);
        Stop();
    }
}

void JSComponent::OnSceneSet(Scene* scene)
{
    if (scene)
        UpdateEventSubscription();
    else
    {
        UnsubscribeFromEvent(E_SCENEUPDATE);
        UnsubscribeFromEvent(E_SCENEPOSTUPDATE);
#ifdef ATOMIC_PHYSICS
        UnsubscribeFromEvent(E_PHYSICSPRESTEP);
        UnsubscribeFromEvent(E_PHYSICSPOSTSTEP);
#endif
        currentEventMask_ = 0;
    }
}

void JSComponent::UpdateEventSubscription()
{
    Scene* scene = GetScene();
    if (!scene)
        return;

    bool enabled = IsEnabledEffective();

    bool needUpdate = enabled && ((updateEventMask_ & USE_UPDATE) || !delayedStartCalled_);
    if (needUpdate && !(currentEventMask_ & USE_UPDATE))
    {
        SubscribeToEvent(scene, E_SCENEUPDATE, HANDLER(JSComponent, HandleSceneUpdate));
        currentEventMask_ |= USE_UPDATE;
    }
    else if (!needUpdate && (currentEventMask_ & USE_UPDATE))
    {
        UnsubscribeFromEvent(scene, E_SCENEUPDATE);
        currentEventMask_ &= ~USE_UPDATE;
    }

    bool needPostUpdate = enabled && (updateEventMask_ & USE_POSTUPDATE);
    if (needPostUpdate && !(currentEventMask_ & USE_POSTUPDATE))
    {
        SubscribeToEvent(scene, E_SCENEPOSTUPDATE, HANDLER(JSComponent, HandleScenePostUpdate));
        currentEventMask_ |= USE_POSTUPDATE;
    }
    else if (!needUpdate && (currentEventMask_ & USE_POSTUPDATE))
    {
        UnsubscribeFromEvent(scene, E_SCENEPOSTUPDATE);
        currentEventMask_ &= ~USE_POSTUPDATE;
    }

#ifdef ATOMIC_PHYSICS
    PhysicsWorld* world = scene->GetComponent<PhysicsWorld>();
    if (!world)
        return;

    bool needFixedUpdate = enabled && (updateEventMask_ & USE_FIXEDUPDATE);
    if (needFixedUpdate && !(currentEventMask_ & USE_FIXEDUPDATE))
    {
        SubscribeToEvent(world, E_PHYSICSPRESTEP, HANDLER(JSComponent, HandlePhysicsPreStep));
        currentEventMask_ |= USE_FIXEDUPDATE;
    }
    else if (!needFixedUpdate && (currentEventMask_ & USE_FIXEDUPDATE))
    {
        UnsubscribeFromEvent(world, E_PHYSICSPRESTEP);
        currentEventMask_ &= ~USE_FIXEDUPDATE;
    }

    bool needFixedPostUpdate = enabled && (updateEventMask_ & USE_FIXEDPOSTUPDATE);
    if (needFixedPostUpdate && !(currentEventMask_ & USE_FIXEDPOSTUPDATE))
    {
        SubscribeToEvent(world, E_PHYSICSPOSTSTEP, HANDLER(JSComponent, HandlePhysicsPostStep));
        currentEventMask_ |= USE_FIXEDPOSTUPDATE;
    }
    else if (!needFixedPostUpdate && (currentEventMask_ & USE_FIXEDPOSTUPDATE))
    {
        UnsubscribeFromEvent(world, E_PHYSICSPOSTSTEP);
        currentEventMask_ &= ~USE_FIXEDPOSTUPDATE;
    }
#endif
}

void JSComponent::HandleSceneUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace SceneUpdate;

    assert(!destroyed_);

    // Execute user-defined delayed start function before first update
    if (!delayedStartCalled_)
    {
        DelayedStart();
        delayedStartCalled_ = true;

        // If did not need actual update events, unsubscribe now
        if (!(updateEventMask_ & USE_UPDATE))
        {
            UnsubscribeFromEvent(GetScene(), E_SCENEUPDATE);
            currentEventMask_ &= ~USE_UPDATE;
            return;
        }
    }

    // Then execute user-defined update function
    Update(eventData[P_TIMESTEP].GetFloat());
}

void JSComponent::HandleScenePostUpdate(StringHash eventType, VariantMap& eventData)
{
    using namespace ScenePostUpdate;

    // Execute user-defined post-update function
    PostUpdate(eventData[P_TIMESTEP].GetFloat());
}

#ifdef ATOMIC_PHYSICS
void JSComponent::HandlePhysicsPreStep(StringHash eventType, VariantMap& eventData)
{
    using namespace PhysicsPreStep;

    // Execute user-defined fixed update function
    FixedUpdate(eventData[P_TIMESTEP].GetFloat());
}

void JSComponent::HandlePhysicsPostStep(StringHash eventType, VariantMap& eventData)
{
    using namespace PhysicsPostStep;

    // Execute user-defined fixed post-update function
    FixedPostUpdate(eventData[P_TIMESTEP].GetFloat());
}
#endif

bool JSComponent::Load(Deserializer& source, bool setInstanceDefault)
{
    loading_ = true;
    bool success = Component::Load(source, setInstanceDefault);
    loading_ = false;

    return success;
}

bool JSComponent::LoadXML(const XMLElement& source, bool setInstanceDefault)
{
    loading_ = true;
    bool success = Component::LoadXML(source, setInstanceDefault);
    loading_ = false;

    return success;
}

void JSComponent::SetComponentFile(JSComponentFile* cfile, bool loading)
{
    componentFile_ = cfile;
}

ResourceRef JSComponent::GetScriptAttr() const
{
    return GetResourceRef(componentFile_, JSComponentFile::GetTypeStatic());
}

void JSComponent::SetScriptAttr(const ResourceRef& value)
{
    ResourceCache* cache = GetSubsystem<ResourceCache>();
    SetComponentFile(cache->GetResource<JSComponentFile>(value.name_), loading_);
}

}
