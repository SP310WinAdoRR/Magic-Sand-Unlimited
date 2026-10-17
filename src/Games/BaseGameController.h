/***********************************************************************
BaseGameController.h - Abstract base class for sandbox games & experiences
***********************************************************************/

#pragma once

#include "ofMain.h"
#include "../KinectProjector/KinectProjector.h"

class BaseGameController {
public:
    BaseGameController() : debugOn(false), projRes(0, 0), kinectRes(640, 480) {}
    virtual ~BaseGameController() {}

    // Lifecycle
    virtual void setup(std::shared_ptr<KinectProjector> const& k) {
        kinectProjector = k;
    }
    virtual void update() = 0;
    virtual bool isIdle() = 0;

    // Shared resolution and ROI setters
    virtual void setProjectorRes(ofVec2f& PR) { projRes = PR; }
    virtual void setKinectRes(ofVec2f& KR)    { kinectRes = KR; }
    virtual void setKinectROI(ofRectangle& KROI) { kinectROI = KROI; }
    virtual ofRectangle getKinectROI() { return kinectROI; }
    virtual void setDebug(bool flag) { debugOn = flag; }

    // Operator screen preview (optional)
    virtual void drawMainWindow(float x, float y, float w, float h) {}

    // Template method for rendering on the projector
    virtual void drawProjectorWindow() {
        if (isIdle()) return;
        drawScene();
        if (isInverted()) {
            ofPushMatrix();
            ofTranslate(projRes.x, projRes.y);
            ofRotate(180.0f);
            drawHUD();
            ofPopMatrix();
        } else {
            drawHUD();
        }
    }

    // Utility helpers for derived games
    bool isInverted() const {
        return kinectProjector && kinectProjector->getIsInverted();
    }

    ofVec2f kinectToProj(float kx, float ky) const {
        return kinectProjector ? kinectProjector->kinectCoordToProjCoord(kx, ky) : ofVec2f(0, 0);
    }

    float getElevation(float kx, float ky) const {
        return kinectProjector ? kinectProjector->elevationAtKinectCoord(kx, ky) : 0.0f;
    }

    float getOrientedAngle(float localAngle) const {
        return isInverted() ? (localAngle + 180.0f) : localAngle;
    }

protected:
    virtual void drawScene() {}
    virtual void drawHUD() {}

    std::shared_ptr<KinectProjector> kinectProjector;
    ofVec2f projRes;
    ofVec2f kinectRes;
    ofRectangle kinectROI;
    bool debugOn;
};

