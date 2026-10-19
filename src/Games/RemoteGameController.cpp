/***********************************************************************
RemoteGameController.cpp - Generic UDP/OSC Bridge for External Sandbox Games
***********************************************************************/

#include "RemoteGameController.h"
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace {
    // Big-endian conversion helpers for OSC 1.0 binary format
    uint32_t toBigEndian32(uint32_t val) {
        return ((val & 0x000000FFU) << 24) |
               ((val & 0x0000FF00U) << 8)  |
               ((val & 0x00FF0000U) >> 8)  |
               ((val & 0xFF000000U) >> 24);
    }

    int32_t readOSCInt32(const uint8_t* ptr) {
        uint32_t netVal = 0;
        std::memcpy(&netVal, ptr, 4);
        uint32_t hostVal = toBigEndian32(netVal);
        int32_t res = 0;
        std::memcpy(&res, &hostVal, 4);
        return res;
    }

    float readOSCFloat32(const uint8_t* ptr) {
        uint32_t netVal = 0;
        std::memcpy(&netVal, ptr, 4);
        uint32_t hostVal = toBigEndian32(netVal);
        float res = 0.0f;
        std::memcpy(&res, &hostVal, 4);
        return res;
    }

    void appendOSCString(std::vector<uint8_t>& buf, const std::string& str) {
        buf.insert(buf.end(), str.begin(), str.end());
        buf.push_back(0);
        while (buf.size() % 4 != 0) {
            buf.push_back(0);
        }
    }

    void appendOSCFloat32(std::vector<uint8_t>& buf, float val) {
        uint32_t raw = 0;
        std::memcpy(&raw, &val, 4);
        uint32_t net = toBigEndian32(raw);
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&net);
        buf.insert(buf.end(), bytes, bytes + 4);
    }

    std::string extractOSCString(const uint8_t* data, size_t len, size_t& offset) {
        if (offset >= len) return "";
        size_t start = offset;
        while (offset < len && data[offset] != 0) {
            offset++;
        }
        std::string s(reinterpret_cast<const char*>(data + start), offset - start);
        offset++; // skip null terminator
        while (offset % 4 != 0 && offset < len) {
            offset++;
        }
        return s;
    }
}

CRemoteGameController::CRemoteGameController()
    : BaseGameController(),
      bridgeActive(false),
      sockTx(-1),
      sockRx(-1),
      remoteIp("127.0.0.1"),
      remoteTxPort(7000),
      localRxPort(7001),
      gridCols(2),
      gridRows(2),
      timerRatio(0.0f),
      timerColor(255, 255, 255, 75),
      showTimer(false),
      currentRound(0),
      currentScore(0),
      centerTitle(""),
      centerSubtitle(""),
      lastSendTime(0.0f),
      sendIntervalSec(1.0f / 30.0f),
      lastPacketReceivedTime(-100.0f)
{
    quadrantElevations.assign(gridCols * gridRows, 0.0f);
    cellStates.resize(gridCols * gridRows);
}

CRemoteGameController::~CRemoteGameController() {
    closeSockets();
}

void CRemoteGameController::setup(std::shared_ptr<KinectProjector> const& k) {
    BaseGameController::setup(k);

    ofTrueTypeFont::setGlobalDpi(72);
    hudFont.loadFont("verdana.ttf", 28);
    titleFont.loadFont("verdana.ttf", 40);
    inspectFont.loadFont("verdana.ttf", 14);

    initSockets("127.0.0.1", 7000, 7001);
}

bool CRemoteGameController::initSockets(const std::string& targetIp, int txPort, int rxPort) {
    closeSockets();
    remoteIp = targetIp;
    remoteTxPort = txPort;
    localRxPort = rxPort;

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // 1. Create TX UDP socket
    sockTx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockTx >= 0) {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sockTx, FIONBIO, &mode);
#else
        int flags = fcntl(sockTx, F_GETFL, 0);
        fcntl(sockTx, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    // 2. Create RX UDP socket bound to localRxPort (7001)
    sockRx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockRx >= 0) {
        int reuse = 1;
        setsockopt(sockRx, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(sockRx, FIONBIO, &mode);
#else
        int flags = fcntl(sockRx, F_GETFL, 0);
        fcntl(sockRx, F_SETFL, flags | O_NONBLOCK);
#endif

        struct sockaddr_in bindAddr;
        std::memset(&bindAddr, 0, sizeof(bindAddr));
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(static_cast<uint16_t>(localRxPort));
        bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);

        if (bind(sockRx, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) < 0) {
            ofLogWarning("RemoteGameController") << "No se pudo enlazar puerto UDP RX " << localRxPort;
        } else {
            ofLogNotice("RemoteGameController") << "Bridge UDP listo: TX -> " << remoteIp << ":" << remoteTxPort
                                                << " | RX <- 0.0.0.0:" << localRxPort;
        }
    }

    return (sockTx >= 0 && sockRx >= 0);
}

void CRemoteGameController::closeSockets() {
    if (sockTx >= 0) {
#ifdef _WIN32
        closesocket(sockTx);
#else
        close(sockTx);
#endif
        sockTx = -1;
    }
    if (sockRx >= 0) {
#ifdef _WIN32
        closesocket(sockRx);
#else
        close(sockRx);
#endif
        sockRx = -1;
    }
}

void CRemoteGameController::toggleBridge() {
    if (bridgeActive) {
        stopBridge();
    } else {
        startBridge();
    }
}

void CRemoteGameController::startBridge() {
    bridgeActive = true;
    ofLogNotice("RemoteGameController") << "Modo Bridge de Juego Remoto ACTIVADO.";
}

void CRemoteGameController::stopBridge() {
    bridgeActive = false;
    ofLogNotice("RemoteGameController") << "Modo Bridge de Juego Remoto DESACTIVADO.";
}

bool CRemoteGameController::isIdle() {
    return !bridgeActive;
}

void CRemoteGameController::setProjectorRes(ofVec2f& PR) {
    BaseGameController::setProjectorRes(PR);
}

void CRemoteGameController::setKinectRes(ofVec2f& KR) {
    BaseGameController::setKinectRes(KR);
}

void CRemoteGameController::setKinectROI(ofRectangle& KROI) {
    BaseGameController::setKinectROI(KROI);
}

void CRemoteGameController::update() {
    // Always poll incoming UDP commands; if an external game sends a packet, auto-activate if needed
    pollIncomingCommands();

    if (!bridgeActive) {
        return;
    }

    float now = ofGetElapsedTimef();
    if (now - lastSendTime >= sendIntervalSec) {
        computeQuadrantElevations();
        sendGridElevationsOSC();
        lastSendTime = now;
    }
}

void CRemoteGameController::computeQuadrantElevations() {
    int numCells = gridCols * gridRows;
    if (static_cast<int>(quadrantElevations.size()) != numCells) {
        quadrantElevations.assign(numCells, 0.0f);
    }

    if (!kinectProjector || kinectROI.width <= 1 || kinectROI.height <= 1) {
        return;
    }

    const int stride = 4; // Sample every 4 pixels for 60 FPS real-time performance
    float cellW = kinectROI.width / static_cast<float>(gridCols);
    float cellH = kinectROI.height / static_cast<float>(gridRows);

    for (int r = 0; r < gridRows; ++r) {
        for (int c = 0; c < gridCols; ++c) {
            int idx = r * gridCols + c;
            float xStart = kinectROI.x + c * cellW;
            float yStart = kinectROI.y + r * cellH;
            float xEnd = xStart + cellW;
            float yEnd = yStart + cellH;

            double sumElevation = 0.0;
            int validSamples = 0;

            for (float ky = yStart + stride; ky < yEnd - stride; ky += stride) {
                for (float kx = xStart + stride; kx < xEnd - stride; kx += stride) {
                    float elev = getElevation(kx, ky);
                    if (std::isfinite(elev) && elev > -300.0f && elev < 300.0f) {
                        sumElevation += elev;
                        validSamples++;
                    }
                }
            }

            quadrantElevations[idx] = (validSamples > 0)
                ? static_cast<float>(sumElevation / validSamples)
                : 0.0f;
        }
    }
}

void CRemoteGameController::sendGridElevationsOSC() {
    if (sockTx < 0) return;

    std::vector<uint8_t> packet;
    appendOSCString(packet, "/sandbox/grid");

    std::string typeTags = ",";
    typeTags.append(quadrantElevations.size(), 'f');
    appendOSCString(packet, typeTags);

    for (size_t i = 0; i < quadrantElevations.size(); ++i) {
        appendOSCFloat32(packet, quadrantElevations[i]);
    }

    struct sockaddr_in destAddr;
    std::memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(static_cast<uint16_t>(remoteTxPort));
    inet_pton(AF_INET, remoteIp.c_str(), &destAddr.sin_addr);

    sendto(
        sockTx,
        reinterpret_cast<const char*>(packet.data()),
        static_cast<int>(packet.size()),
        0,
        reinterpret_cast<struct sockaddr*>(&destAddr),
        sizeof(destAddr)
    );
}

void CRemoteGameController::pollIncomingCommands() {
    if (sockRx < 0) return;

    uint8_t buffer[2048];
    struct sockaddr_in senderAddr;
    socklen_t senderLen = sizeof(senderAddr);

    while (true) {
        int received = recvfrom(
            sockRx,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<struct sockaddr*>(&senderAddr),
            &senderLen
        );
        if (received <= 0) {
            break;
        }
        lastPacketReceivedTime = ofGetElapsedTimef();
        parseOSCPacket(buffer, static_cast<size_t>(received));
    }
}

void CRemoteGameController::parseOSCPacket(const uint8_t* data, size_t len) {
    if (len < 4 || data[0] != '/') return;

    size_t offset = 0;
    std::string address = extractOSCString(data, len, offset);
    std::string tags = extractOSCString(data, len, offset);
    if (tags.empty() || tags[0] != ',') return;

    // Parse arguments sequentially
    struct Arg {
        char type;
        int32_t i;
        float f;
        std::string s;
    };
    std::vector<Arg> args;

    for (size_t t = 1; t < tags.size(); ++t) {
        char c = tags[t];
        Arg a;
        a.type = c;
        a.i = 0;
        a.f = 0.0f;
        if (c == 'i') {
            if (offset + 4 <= len) {
                a.i = readOSCInt32(data + offset);
                a.f = static_cast<float>(a.i);
                offset += 4;
                args.push_back(a);
            }
        } else if (c == 'f') {
            if (offset + 4 <= len) {
                a.f = readOSCFloat32(data + offset);
                a.i = static_cast<int32_t>(a.f);
                offset += 4;
                args.push_back(a);
            }
        } else if (c == 's') {
            a.s = extractOSCString(data, len, offset);
            args.push_back(a);
        }
    }

    if (address == "/render/grid/setup" && args.size() >= 2) {
        int newCols = std::max(1, args[0].i);
        int newRows = std::max(1, args[1].i);
        if (newCols != gridCols || newRows != gridRows) {
            gridCols = newCols;
            gridRows = newRows;
            quadrantElevations.assign(gridCols * gridRows, 0.0f);
            cellStates.assign(gridCols * gridRows, CellVisualState());
        }
        if (!bridgeActive) {
            bridgeActive = true;
        }
    } else if (address == "/render/grid/cell" && args.size() >= 5) {
        int idx = args[0].i;
        if (idx >= 0 && idx < static_cast<int>(cellStates.size())) {
            cellStates[idx].baseColor = ofColor(
                ofClamp(args[1].i, 0, 255),
                ofClamp(args[2].i, 0, 255),
                ofClamp(args[3].i, 0, 255)
            );
            cellStates[idx].alpha = ofClamp(args[4].f * 255.0f, 0.0f, 255.0f);
            cellStates[idx].innerScale = 0.0f;
            if (args.size() >= 6) {
                cellStates[idx].label = args[5].s;
            }
        }
    } else if (address == "/render/grid/cell_progress" && args.size() >= 8) {
        int idx = args[0].i;
        if (idx >= 0 && idx < static_cast<int>(cellStates.size())) {
            cellStates[idx].baseColor = ofColor(
                ofClamp(args[1].i, 0, 255),
                ofClamp(args[2].i, 0, 255),
                ofClamp(args[3].i, 0, 255)
            );
            cellStates[idx].innerColor = ofColor(
                ofClamp(args[4].i, 0, 255),
                ofClamp(args[5].i, 0, 255),
                ofClamp(args[6].i, 0, 255)
            );
            cellStates[idx].innerScale = ofClamp(args[7].f, 0.0f, 1.0f);
        }
    } else if (address == "/render/hud/timer" && args.size() >= 1) {
        timerRatio = ofClamp(args[0].f, 0.0f, 1.0f);
        showTimer = (timerRatio > 0.0f);
        if (args.size() >= 4) {
            int alpha = (args.size() >= 5) ? ofClamp(args[4].i, 0, 255) : 75;
            timerColor = ofColor(
                ofClamp(args[1].i, 0, 255),
                ofClamp(args[2].i, 0, 255),
                ofClamp(args[3].i, 0, 255),
                alpha
            );
        }
    } else if (address == "/render/hud/stats" && args.size() >= 2) {
        currentRound = args[0].i;
        currentScore = args[1].i;
    } else if (address == "/render/hud/title" && args.size() >= 1) {
        centerTitle = args[0].s;
        centerSubtitle = (args.size() >= 2) ? args[1].s : "";
    } else if (address == "/render/clear") {
        showTimer = false;
        centerTitle = "";
        centerSubtitle = "";
        for (size_t i = 0; i < cellStates.size(); ++i) {
            cellStates[i] = CellVisualState();
        }
    }
}

void CRemoteGameController::drawScene() {
    renderGameViewport(0.0f, 0.0f, projRes.x, projRes.y, false);
}

void CRemoteGameController::drawHUD() {
    float width = projRes.x;
    float height = projRes.y;

    ofPushStyle();
    ofEnableAlphaBlending();

    // 1. Central translucent pie timer ("torta")
    if (showTimer && timerRatio > 0.001f) {
        float radius = std::min(width, height) * 0.20f;
        drawPieTimer(width * 0.5f, height * 0.5f, radius, timerRatio);
    }

    // 2. Superimposed Round & Score inside safe zone
    float safeX = width * 0.12f;
    float safeY = height * 0.10f;

    if (currentRound > 0 && hudFont.isLoaded()) {
        std::string lvlStr = "NIVEL " + ofToString(currentRound);
        std::string ptsStr = ofToString(currentScore) + " PTS";

        ofSetColor(0, 0, 0, 100);
        hudFont.drawString(lvlStr, safeX + 2, safeY + 2);
        float pw = hudFont.stringWidth(ptsStr);
        hudFont.drawString(ptsStr, width - safeX - pw + 2, safeY + 2);

        ofSetColor(255, 255, 255, 140);
        hudFont.drawString(lvlStr, safeX, safeY);
        hudFont.drawString(ptsStr, width - safeX - pw, safeY);
    }

    // 3. Centered translucent modal prompt (Welcome / Flatten sand / Game Over)
    if (!centerTitle.empty() && titleFont.isLoaded()) {
        float tw = titleFont.stringWidth(centerTitle);
        float th = titleFont.stringHeight(centerTitle);
        float cx = width * 0.5f;
        float cy = height * 0.5f;

        ofSetColor(0, 0, 0, 70);
        ofDrawRectRounded(cx - tw * 0.5f - 22.0f, cy - th * 0.5f - 14.0f, tw + 44.0f, th + 28.0f, 10.0f);

        ofSetColor(0, 0, 0, 120);
        titleFont.drawString(centerTitle, cx - tw * 0.5f + 2.0f, cy + th * 0.35f + 2.0f);
        ofSetColor(255, 255, 255, 180);
        titleFont.drawString(centerTitle, cx - tw * 0.5f, cy + th * 0.35f);
    }

    ofDisableAlphaBlending();
    ofPopStyle();
}

void CRemoteGameController::renderGameViewport(float x, float y, float w, float h, bool /*includeHud*/) {
    if (gridCols <= 0 || gridRows <= 0) return;

    ofPushStyle();
    ofEnableAlphaBlending();

    // 1. Edge-to-edge quadrants
    for (int r = 0; r < gridRows; ++r) {
        for (int c = 0; c < gridCols; ++c) {
            int idx = r * gridCols + c;
            float x0 = x + (c * w / static_cast<float>(gridCols));
            float x1 = x + ((c + 1) * w / static_cast<float>(gridCols));
            float y0 = y + (r * h / static_cast<float>(gridRows));
            float y1 = y + ((r + 1) * h / static_cast<float>(gridRows));
            float cw = x1 - x0;
            float ch = y1 - y0;

            CellVisualState state = (idx < static_cast<int>(cellStates.size()))
                ? cellStates[idx]
                : CellVisualState();

            ofFill();
            ofSetColor(state.baseColor.r, state.baseColor.g, state.baseColor.b, static_cast<int>(state.alpha));
            ofDrawRectangle(x0, y0, cw, ch);

            if (state.innerScale > 0.005f) {
                float iw = cw * state.innerScale;
                float ih = ch * state.innerScale;
                ofSetColor(state.innerColor.r, state.innerColor.g, state.innerColor.b, static_cast<int>(state.alpha));
                ofDrawRectangle(x0 + (cw - iw) * 0.5f, y0 + (ch - ih) * 0.5f, iw, ih);
            }
        }
    }

    // 2. Solid black dividing lines
    ofSetColor(0, 0, 0, 255);
    ofSetLineWidth(5.0f);
    for (int c = 1; c < gridCols; ++c) {
        float lx = x + (c * w / static_cast<float>(gridCols));
        ofDrawLine(lx, y, lx, y + h);
    }
    for (int r = 1; r < gridRows; ++r) {
        float ly = y + (r * h / static_cast<float>(gridRows));
        ofDrawLine(x, ly, x + w, ly);
    }

    ofDisableAlphaBlending();
    ofPopStyle();
}

void CRemoteGameController::drawPieTimer(float cx, float cy, float radius, float ratio) {
    // Subtle dark circle reference
    ofFill();
    ofSetColor(0, 0, 0, 35);
    ofDrawCircle(cx, cy, radius);

    // Translucent pie slice
    int numSegments = std::max(3, static_cast<int>(90 * ratio));
    float startAngle = -HALF_PI;
    float endAngle = startAngle + (TWO_PI * ratio);

    if (ratio > 0.25f) {
        ofSetColor(255, 255, 255, 75);
    } else {
        ofSetColor(255, 235, 80, 95);
    }

    ofBeginShape();
    ofVertex(cx, cy);
    for (int i = 0; i <= numSegments; ++i) {
        float ang = startAngle + (endAngle - startAngle) * (static_cast<float>(i) / numSegments);
        ofVertex(cx + radius * std::cos(ang), cy + radius * std::sin(ang));
    }
    ofEndShape( true );

    // Outer ring
    ofNoFill();
    ofSetLineWidth(3.0f);
    ofSetColor(0, 0, 0, 65);
    ofDrawCircle(cx, cy, radius);
}

void CRemoteGameController::drawMainWindow(float x, float y, float w, float h) {
    if (!bridgeActive) return;

    ofPushStyle();
    ofEnableAlphaBlending();

    // Draw translucent preview of game quadrants over the operator's sand view
    for (int r = 0; r < gridRows; ++r) {
        for (int c = 0; c < gridCols; ++c) {
            int idx = r * gridCols + c;
            float x0 = x + (c * w / static_cast<float>(gridCols));
            float x1 = x + ((c + 1) * w / static_cast<float>(gridCols));
            float y0 = y + (r * h / static_cast<float>(gridRows));
            float y1 = y + ((r + 1) * h / static_cast<float>(gridRows));
            float cw = x1 - x0;
            float ch = y1 - y0;

            if (idx < static_cast<int>(cellStates.size()) && cellStates[idx].innerScale > 0.01f) {
                ofFill();
                ofColor ic = cellStates[idx].innerColor;
                ofSetColor(ic.r, ic.g, ic.b, 90);
                ofDrawRectangle(x0, y0, cw, ch);
            }

            // Operator-only inspection telemetry per quadrant (exact mm height)
            float elev = (idx < static_cast<int>(quadrantElevations.size())) ? quadrantElevations[idx] : 0.0f;
            std::string info = "Q" + ofToString(idx + 1) + ": " + ofToString(elev, 1) + " mm";
            ofDrawBitmapStringHighlight(info, x0 + 12.0f, y0 + 24.0f, ofColor(0, 0, 0, 180), ofColor(255, 255, 255));
        }
    }

    // Draw quadrant grid lines on operator monitor
    ofSetColor(0, 0, 0, 220);
    ofSetLineWidth(3.0f);
    for (int c = 1; c < gridCols; ++c) {
        float lx = x + (c * w / static_cast<float>(gridCols));
        ofDrawLine(lx, y, lx, y + h);
    }
    for (int r = 1; r < gridRows; ++r) {
        float ly = y + (r * h / static_cast<float>(gridRows));
        ofDrawLine(x, ly, x + w, ly);
    }

    // Operator status banner
    bool clientConnected = (ofGetElapsedTimef() - lastPacketReceivedTime) < 3.0f;
    std::string status = "BRIDGE UDP [ACTIVO] | TX->127.0.0.1:" + ofToString(remoteTxPort) +
                         " | RX<-:" + ofToString(localRxPort) +
                         " | Cliente: " + (clientConnected ? "CONECTADO" : "ESPERANDO") +
                         " | [B] Desactivar";
    ofDrawBitmapStringHighlight(status, x + 10.0f, y + h - 12.0f,
                                clientConnected ? ofColor(0, 130, 70, 220) : ofColor(40, 40, 40, 220),
                                ofColor(255, 255, 255));

    ofDisableAlphaBlending();
    ofPopStyle();
}
