/** 
 * Activity using OpenNI/NiTE to track hands
 */

#include <node.h>
#include <v8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <math.h>

#include "NiTE.h"
#include "Main.h"
#include "../enums/Skeleton.h"
#include "../enums/Joint.h"
#include "../enums/GestureTypes.h"
#include "../enums/EventTypes.h"
#include "../gestures/GestureRecognizer.c"
#include "../Common/NiteSampleUtilities.h"

using namespace v8;

#define PI 3.141592

#define MAX_USERS 1

bool g_visibleUsers[MAX_USERS] = {false};
nite::SkeletonState g_skeletonStates[MAX_USERS] = {nite::SKELETON_NONE};

/**
 * entry point for plugin
 *
 * @param plugin target
 */
void init(Handle<Object> target) {
    fprintf(stderr, "Nui Motion Initialize \n");
    target->Set(String::NewSymbol("init"),
        FunctionTemplate::New(initialize)->GetFunction());
    target->Set(String::NewSymbol("close"),
        FunctionTemplate::New(close)->GetFunction());
    target->Set(String::NewSymbol("getJoints"),
        FunctionTemplate::New(getJoints)->GetFunction());

    context_obj = Persistent<Object>::New(Object::New()); 
    target->Set(String::New("context"), context_obj); 

    gst = GestureRecognizer();
}


/**
 * JS Call to get joints
 * @param args - each parameter is one named joint
 * @return object structure of joints
 */
Handle<Value> getJoints(const Arguments& args) {
    HandleScope scope;

    Local<Object> skel_obj = Object::New(); 
    for ( int c = 0; c < args.Length(); c++)
    {   
        if (!args[c]->IsString()) {
            ThrowException(Exception::TypeError(String::New("Argument needs to be a string")));
            return scope.Close(Undefined());
        }

        String::Utf8Value utfStr(args[c]->ToString());
        char* s = (char*) *utfStr;
        Local<Object> joint = mapJointToJSObject(s);
        skel_obj->Set(String::NewSymbol(s), joint ); 
    }
    return scope.Close(skel_obj);
}


/**
 * on device event
 *
 * @param event type
 */
void onDeviceEvent(int eventType) {
    Local <String> eventString;
    switch (eventType) {
        case DEVICE_INITIALIZED:
            eventString = String::New("DEVICE_INITIALIZED");
            break;
        case DEVICE_ERROR:
            eventString = String::New("DEVICE_ERROR");
            break;
        default:
            eventString = String::New("No known event type found");
            break;
    }
    Local<Value> args[] = { eventString }; 
    node::MakeCallback(context_obj, "on", 1, args);
}

/**
 * on tracking event found in frame processing thread
 * we message out to Node in here
 *
 * @param async handle
 * @param status (?)
 */
void onTrackingEvent(uv_async_t *handle, int status /*UNUSED*/) {
    int event = *((int*) handle->data);
    Local <String> eventString;
    switch (event) {
        case NEW_USER:
            eventString = String::New("NEW_USER");
            lastUserEventDispatched = currentUserState;
            break;
        case USER_IS_VISIBLE:
            eventString = String::New("USER_IS_VISIBLE");
            lastUserEventDispatched = currentUserState;
            break; 
        case USER_IS_OUT_OF_SCENE:
            eventString = String::New("USER_IS_OUT_OF_SCENE");
            lastUserEventDispatched = currentUserState;
            break; 
        case USER_IS_LOST:
            eventString = String::New("USER_IS_LOST");
            lastUserEventDispatched = currentUserState;
            break; 
        case SKELETON_STOPPED_TRACKING:
            eventString = String::New("SKELETON_STOPPED_TRACKING");
            lastSkeletalEventDispatched = currentSkeletonState;
            break; 
        case SKELETON_TRACKING:
            eventString = String::New("SKELETON_TRACKING");
            lastSkeletalEventDispatched = currentSkeletonState;
            break; 
        case SKELETON_CALIBRATING:
            eventString = String::New("SKELETON_CALIBRATING");
            lastSkeletalEventDispatched = currentSkeletonState;
            break; 
        case SKELETON_CALIBRATION_FAILED:
            eventString = String::New("SKELETON_CALIBRATION_FAILED");
            lastSkeletalEventDispatched = currentSkeletonState;
            break; 
        case SWIPE_LEFT:
            eventString = String::New("GESTURE_SWIPE_LEFT");
            break;
        case SWIPE_RIGHT:
            eventString = String::New("GESTURE_SWIPE_RIGHT");
            break;
        case SWIPE_UP:
            eventString = String::New("GESTURE_SWIPE_UP");
            break;
        case SWIPE_DOWN:
            eventString = String::New("GESTURE_SWIPE_DOWN");
            break;
        case WAVE:
            eventString = String::New("GESTURE_WAVE");
            break;

        default:
            fprintf(stderr, "Event type error %d \n", event);
            eventString = String::New("No known event type found");
            break; 
    }

    Local<Value> args[] = { eventString }; 
    node::MakeCallback(context_obj, "on", 1, args);
}


/** 
 * shutdown/cleanup NITE/OpenNI
 * 
 * @param args (none used)
 */
Handle<Value> close(const Arguments& args) {
    HandleScope scope;
    fprintf(stderr, "Shutdown NITE\n");
    keepWorkerRunning = false;
    nite::NiTE::shutdown();
    return scope.Close(Undefined());
}

/**
 * nodeJS method to intialize and start OpenNI/NiTE
 *
 * @param args (none - don't pass them in here)
 */
Handle<Value> initialize(const Arguments& args) {
    HandleScope scope;

    nite::Status niteRc;

    fprintf(stderr, "Initialize Depth Camera\n");
    
    niteRc = nite::NiTE::initialize();
    if (niteRc != nite::STATUS_OK)
    {
        fprintf(stderr, "NiTE initialization failed\n");
        onDeviceEvent(DEVICE_ERROR);
        return scope.Close( Undefined() );
    }

    niteRc = userTracker.create();
    if (niteRc != nite::STATUS_OK)
    {
        fprintf(stderr, "Couldn't create user tracker\n");
        onDeviceEvent(DEVICE_ERROR);
        return scope.Close(Undefined());
    }
    fprintf(stderr,"Start moving around to get detected...\n(PSI pose may be required for skeleton calibration, depending on the configuration)\n");

    niteRc = handTracker.create();
    if (niteRc != nite::STATUS_OK)
    {
        fprintf(stderr, "Couldn't create hand tracker\n");
        return scope.Close( Undefined() );
    }
    handTracker.startGestureDetection(nite::GESTURE_WAVE);

    keepWorkerRunning = true;
    skeleton.leftHand.isActive = false;
    skeleton.leftHand.type = nite::JOINT_LEFT_HAND;
    skeleton.leftElbow.isActive = false;
    skeleton.leftElbow.type = nite::JOINT_LEFT_ELBOW;
    skeleton.leftShoulder.isActive = false;
    skeleton.leftShoulder.type = nite::JOINT_LEFT_SHOULDER;

    skeleton.rightHand.isActive = false;
    skeleton.rightHand.type = nite::JOINT_RIGHT_HAND;
    skeleton.rightElbow.isActive = false;
    skeleton.rightElbow.type = nite::JOINT_RIGHT_ELBOW;
    skeleton.rightShoulder.type = nite::JOINT_RIGHT_SHOULDER;

    skeleton.torso.isActive = false;
    skeleton.torso.type = nite::JOINT_TORSO;

    skeleton.head.isActive = false;
    skeleton.head.type = nite::JOINT_HEAD;

    skeleton.leftHip.isActive = false;
    skeleton.leftHip.type = nite::JOINT_LEFT_HIP;

    skeleton.rightHip.isActive = false;
    skeleton.rightHip.type = nite::JOINT_RIGHT_HIP;

    onDeviceEvent(DEVICE_INITIALIZED);
    
    loop = uv_default_loop();

    uv_work_t req;
    uv_async_init(loop, &async, onTrackingEvent);
    uv_queue_work(loop, &req, frameWorker, onFrameWorkerThreadComplete);
    uv_run(loop);

    return scope.Close(Undefined());
}

/**
 * thread worker thread for OpenNI/NITE to read frames and do work on them
 *
 * @param thread request
 */
void onFrameWorkerThreadComplete(uv_work_t *req) {
    fprintf(stderr, "OpenNI/NITE Processing Complete\n");
    uv_close((uv_handle_t*) &async, NULL);
} 

/**
 * process frames in separate thread
 *
 * @param request thread 
 */
void frameWorker(uv_work_t *req) {
    int userEventType = 0;
    int gestureType = 0;
    while (keepWorkerRunning) {
        nite::Status niteRc;
        nite::UserTrackerFrameRef userTrackerFrame;
        niteRc = userTracker.readFrame(&userTrackerFrame);

        if (niteRc != nite::STATUS_OK)
        {
            fprintf(stderr,"Get next frame failed\n");
            continue;
        }

        nite::HandTrackerFrameRef handTrackerFrame;
        niteRc = handTracker.readFrame(&handTrackerFrame);
        if (niteRc != nite::STATUS_OK)
        {
            fprintf(stderr,"Hand track frame failed\n");
            continue;
        }

        const nite::Array<nite::UserData>& users = userTrackerFrame.getUsers();
        for (int i = 0; i < users.getSize(); ++i)
        {
            const nite::UserData& user = users[i];

            updateUserState(user,userTrackerFrame.getTimestamp());
            if (user.isNew())
            {
                userTracker.startSkeletonTracking(user.getId());
                skeleton.leftHand.isActive = false;
                skeleton.leftElbow.isActive = false;
                skeleton.leftShoulder.isActive = false;
                skeleton.rightHand.isActive = false;
                skeleton.rightElbow.isActive = false;
                skeleton.rightShoulder.isActive = false;
                skeleton.head.isActive = false;
                skeleton.torso.isActive = false;
                skeleton.rightHip.isActive = false;
                skeleton.leftHip.isActive = false;
            } 
            else if (user.getSkeleton().getState() == nite::SKELETON_TRACKED)
            {
                const nite::Skeleton &niteskeleton = user.getSkeleton();
                mapSkeleton(skeleton, niteskeleton);
                gestureType = gst.updateSkeleton(skeleton);

                if (gestureType != NO_GESTURE) {
                    async.data = (void*) &gestureType;
                    uv_async_send(&async);
                }
            }
        }

        const nite::Array<nite::GestureData>& gestures = handTrackerFrame.getGestures();
        for (int i = 0; i < gestures.getSize(); ++i)
        {
            if (gestures[i].isComplete())
            {
                if (gestures[i].getType() == nite::GESTURE_WAVE) {
                    gestureType = WAVE;
                    async.data = (void*) &gestureType;
                    uv_async_send(&async);
                }
            }
        }
    }
}

Local<Object> mapJointToJSObject(char *jointName) {
    Local<Object> jsJoint = Object::New();

    int left_percentExtended = -1;
    int right_percentExtended = -1;
    Joint j;

    if (strcmp( jointName, "torso") == 0) {
        j = skeleton.torso; 
    } else if (strcmp( jointName, "head") == 0) {
        j = skeleton.head; 
    } else if (strcmp( jointName, "left_hand") == 0) {
        j = skeleton.leftHand; 
        float dist_lh2le = sqrt( pow(skeleton.leftHand.xPos - skeleton.leftElbow.xPos, 2) + pow(skeleton.leftHand.yPos- skeleton.leftElbow.yPos, 2) + pow(skeleton.leftHand.zPos - skeleton.leftElbow.zPos, 2) );
        float dist_lh2ls = sqrt( pow(skeleton.leftHand.xPos - skeleton.leftShoulder.xPos, 2) + pow(skeleton.leftHand.yPos - skeleton.leftShoulder.yPos, 2) + pow(skeleton.leftHand.zPos - skeleton.leftShoulder.zPos, 2) );
        float dist_le2ls = sqrt( pow(skeleton.leftElbow.xPos - skeleton.leftShoulder.xPos, 2) + pow(skeleton.leftElbow.yPos - skeleton.leftShoulder.yPos, 2) + pow(skeleton.leftElbow.zPos - skeleton.leftShoulder.zPos, 2) );
        left_percentExtended = (int) dist_lh2ls/(dist_le2ls + dist_lh2le) *100;
    } else if (strcmp( jointName, "left_elbow") == 0) {
        j = skeleton.leftElbow; 
    } else if (strcmp( jointName, "left_shoulder") == 0) {
        j = skeleton.leftElbow; 
    } else if (strcmp( jointName, "left_hip") == 0) {
        j = skeleton.leftHip; 
    } else if (strcmp( jointName, "right_hand") == 0) {
        float dist_rh2re = sqrt( pow(skeleton.rightHand.xPos - skeleton.rightElbow.xPos, 2) + pow(skeleton.rightHand.yPos- skeleton.rightElbow.yPos, 2) + pow(skeleton.rightHand.zPos - skeleton.rightElbow.zPos, 2) );
        float dist_rh2rs = sqrt( pow(skeleton.rightHand.xPos - skeleton.rightShoulder.xPos, 2) + pow(skeleton.rightHand.yPos - skeleton.rightShoulder.yPos, 2) + pow(skeleton.rightHand.zPos - skeleton.rightShoulder.zPos, 2) );
        float dist_re2rs = sqrt( pow(skeleton.rightElbow.xPos - skeleton.rightShoulder.xPos, 2) + pow(skeleton.rightElbow.yPos - skeleton.rightShoulder.yPos, 2) + pow(skeleton.rightElbow.zPos - skeleton.rightShoulder.zPos, 2) );
        right_percentExtended = (int) dist_rh2rs/(dist_re2rs + dist_rh2re) *100;
        j = skeleton.rightHand; 
    } else if (strcmp( jointName, "right_elbow") == 0) {
        j = skeleton.rightElbow; 
    } else if (strcmp( jointName, "right_shoulder") == 0) {
        j = skeleton.rightElbow; 
    } else if (strcmp( jointName, "right_hip") == 0) {
        j = skeleton.rightHip; 
    } else {
        j = Joint();
    }

    jsJoint->Set(String::NewSymbol("x"), Number::New( j.xPos ));
    jsJoint->Set(String::NewSymbol("y"), Number::New( j.yPos ));
    jsJoint->Set(String::NewSymbol("z"), Number::New( j.zPos ));
    jsJoint->Set(String::NewSymbol("xRotation"), Number::New( j.xRotation ));
    jsJoint->Set(String::NewSymbol("yRotation"), Number::New( j.yRotation ));
    jsJoint->Set(String::NewSymbol("zRotation"), Number::New( j.zRotation ));

    if (j.type == nite::JOINT_LEFT_HAND) {
       jsJoint->Set(String::NewSymbol("percentExtended"), Number::New(left_percentExtended)); 
    } else if (j.type == nite::JOINT_RIGHT_HAND) {
       jsJoint->Set(String::NewSymbol("percentExtended"), Number::New(right_percentExtended));  
    }

    jsJoint->Set(String::NewSymbol("active"), Number::New( j.isActive ));

    return jsJoint;
}

/**
 * map skeleton to internal skeleyon struct
 *
 * Skeleton skeleton
 * nite::Skeleton niteskeleton
 */
void mapSkeleton(Skeleton &skeleton, nite::Skeleton niteskeleton) {
    mapJointFromSkeleton(skeleton.leftHand, niteskeleton);
    mapJointFromSkeleton(skeleton.leftElbow, niteskeleton);
    mapJointFromSkeleton(skeleton.leftShoulder, niteskeleton);
    mapJointFromSkeleton(skeleton.rightHand, niteskeleton);
    mapJointFromSkeleton(skeleton.rightElbow, niteskeleton);
    mapJointFromSkeleton(skeleton.rightShoulder, niteskeleton);
    mapJointFromSkeleton(skeleton.torso, niteskeleton);
    mapJointFromSkeleton(skeleton.head, niteskeleton);
    mapJointFromSkeleton(skeleton.leftHip, niteskeleton);
    mapJointFromSkeleton(skeleton.rightHip, niteskeleton);
}

/**
 * map joint properties from same joint on skeleton
 *
 * Joint j
 * Skeleton s
 */
void mapJointFromSkeleton(Joint &j, nite::Skeleton s) {
    j.xPos = (int) s.getJoint( (nite::JointType) j.type).getPosition().x;
    j.yPos = (int) s.getJoint( (nite::JointType) j.type).getPosition().y;
    j.zPos = (int) s.getJoint( (nite::JointType) j.type).getPosition().z;

    const nite::Quaternion &o = s.getJoint( (nite::JointType) j.type).getOrientation();

    j.xRotation = atan2(2*o.y*o.w-2*o.x*o.z , 1 - 2*pow(o.y,2) - 2*pow(o.z, 2)) * (180/PI);
    j.yRotation = asin(2*o.x*o.y + 2*o.z*o.w) * (180/PI);
    j.zRotation = atan2(2*o.x*o.w-2*o.y*o.z , 1 - 2*pow(o.x, 2) - 2*pow(o.z, 2)) * (180/PI);

    if (s.getJoint( (nite::JointType) j.type).getPositionConfidence() > .5) {
        j.isActive = true;
    } else {
        j.isActive = false;
    }
}

/**
 * update and event out on state of user and skeleton tracking
 */
void updateUserState(const nite::UserData& user, unsigned long long ts)
{
    if (user.isNew()) {
        currentUserState = NEW_USER;
    } else if (user.isVisible() && !g_visibleUsers[user.getId()]) {
        currentUserState = USER_IS_VISIBLE;
    } else if (!user.isVisible() && g_visibleUsers[user.getId()]) {
        currentUserState = USER_IS_OUT_OF_SCENE;
    } else if (user.isLost()) {
        currentUserState = USER_IS_LOST;
    }

    if (currentUserState != 0 && lastUserEventDispatched != currentUserState) {
        async.data = (void*) &currentUserState;
        uv_async_send(&async);
    }

    g_visibleUsers[user.getId()] = user.isVisible();

    if(g_skeletonStates[user.getId()] != user.getSkeleton().getState())
    {
        switch(g_skeletonStates[user.getId()] = user.getSkeleton().getState())
        {
        case nite::SKELETON_NONE:
            currentSkeletonState = SKELETON_STOPPED_TRACKING;
            break;
        case nite::SKELETON_CALIBRATING:
            currentSkeletonState = SKELETON_CALIBRATING;
            break;
        case nite::SKELETON_TRACKED:
            currentSkeletonState = SKELETON_TRACKING;
            break;
        case nite::SKELETON_CALIBRATION_ERROR_NOT_IN_POSE:
        case nite::SKELETON_CALIBRATION_ERROR_HANDS:
        case nite::SKELETON_CALIBRATION_ERROR_LEGS:
        case nite::SKELETON_CALIBRATION_ERROR_HEAD:
        case nite::SKELETON_CALIBRATION_ERROR_TORSO:
            currentSkeletonState = SKELETON_CALIBRATION_FAILED;
            break;
        }
    }

    if (currentSkeletonState!= 0 && lastSkeletalEventDispatched != currentSkeletonState) {
        async.data = (void*) &currentSkeletonState;
        uv_async_send(&async);
    }
}

/* Module Declaration */
NODE_MODULE(nuimotion, init)
