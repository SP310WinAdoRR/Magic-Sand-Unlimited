/***********************************************************************
RemoteGameController.h - Generic UDP/OSC Bridge for External Sandbox Games
Allows external games (Python/Pygame, Godot, etc.) to receive real-time
sand elevation data (port 7000) and send projection render commands (port 7001).
***********************************************************************/

#pragma once

#include "ofMain.h"
#include "ofxDatGui.h"
#include "BaseGameController.h"
#include <vector>
#include <string>
#include <stdint.h>

class CRemoteGameController : public BaseGameController {
public:
    struct CellVisualState {
        ofColor baseColor;
        ofColor innerColor;
        float alpha;
        float innerScale; // 0.0 = only baseColor, (0..1) = concentric inner rect of innerColor
        std::string label;

        CellVisualState()
            : baseColor(0, 184, 220),
              innerColor(255, 30, 39),
              alpha(255.0f),
              innerScale(0.0f),
              label("") {}
    };

    CRemoteGameController();
    virtual ~CRemoteGameController();

    void setup(std::shared_ptr<KinectProjector> const& k) override;
    void update() override;
    bool isIdle() override;

    void drawMainWindow(float x, float y, float w, float h) override;
    void setProjectorRes(ofVec2f& PR) override;
    void setKinectRes(ofVec2f& KR) override;
    void setKinectROI(ofRectangle& KROI) override;

    void setupGui();
    void toggleBridge();
    void startBridge();
    void stopBridge();
    bool isBridgeActive() const { return bridgeActive; }

protected:
    void drawScene() override;
    void drawHUD() override;

private:
    void onToggleEvent(ofxDatGuiToggleEvent e);
    void onButtonEvent(ofxDatGuiButtonEvent e);

    // Socket setup & teardown
    bool initSockets(const std::string& targetIp = "127.0.0.1", int txPort = 7000, int rxPort = 7001);
    void closeSockets();

    // Sand elevation sampling & OSC transmission
    void computeQuadrantElevations();
    void sendGridElevationsOSC();

    // Non-blocking OSC reception & parsing
    void pollIncomingCommands();
    void parseOSCPacket(const uint8_t* data, size_t len);

    // Helper for drawing the translucent chronometer and framing within calibrated projector ROI
    ofRectangle getCalibratedProjBounds() const;
    void drawPieTimer(float cx, float cy, float radius, float ratio);
    void renderGameViewport(float x, float y, float w, float h, bool includeHud);

    ofxDatGui* gui;
    bool bridgeActive;
    int sockTx;
    int sockRx;
    std::string remoteIp;
    int remoteTxPort;
    int localRxPort;

    int gridCols;
    int gridRows;
    std::vector<float> quadrantElevations;
    std::vector<CellVisualState> cellStates;

    // HUD / Overlay state received from external game
    float timerRatio;
    ofColor timerColor;
    bool showTimer;
    int currentRound;
    int currentScore;
    std::string centerTitle;
    std::string centerSubtitle;

    float lastSendTime;
    float sendIntervalSec;
    float lastPacketReceivedTime;

    ofTrueTypeFont hudFont;
    ofTrueTypeFont titleFont;
    ofTrueTypeFont inspectFont;
};
