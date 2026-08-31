//=============================================================================
// Copyright © 2026 NaturalPoint, Inc. All Rights Reserved.
//
// THIS SOFTWARE IS GOVERNED BY THE OPTITRACK PLUGINS EULA AVAILABLE AT
// https://www.optitrack.com/about/legal/eula.html AND/OR FOR DOWNLOAD WITH THE
// APPLICABLE SOFTWARE FILE(S) (“PLUGINS EULA”). BY DOWNLOADING, INSTALLING,
// ACTIVATING AND/OR OTHERWISE USING THE SOFTWARE, YOU ARE AGREEING THAT YOU
// HAVE READ, AND THAT YOU AGREE TO COMPLY WITH AND ARE BOUND BY, THE PLUGINS
// EULA AND ALL APPLICABLE LAWS AND REGULATIONS. IF YOU DO NOT AGREE TO BE BOUND
// BY THE PLUGINS EULA, THEN YOU MAY NOT DOWNLOAD, INSTALL, ACTIVATE OR
// OTHERWISE USE THE SOFTWARE AND YOU MUST PROMPTLY DELETE OR RETURN IT. IF YOU
// ARE DOWNLOADING, INSTALLING, ACTIVATING AND/OR OTHERWISE USING THE SOFTWARE
// ON BEHALF OF AN ENTITY, THEN BY DOING SO YOU REPRESENT AND WARRANT THAT YOU
// HAVE THE APPROPRIATE AUTHORITY TO ACCEPT THE PLUGINS EULA ON BEHALF OF SUCH
// ENTITY. See license file in root directory for additional governing terms and
// information.
//=============================================================================

/*********************************************************************
 * \page   CalibHealthSystem.cpp
 * \files   CalibHealthSystem.cpp, CalibHealthSystem.h
 * \brief  Calib Health Client using NatNet Library
 * This program connects to a NatNet server, recieves a data stream, 
 * and allows for user interactivity for monitoring the health of 
 * calibration via markers, rigid bodies, anchor markers, and cameras
 * in NatNet. The purpose of this application is to provide an example 
 * of health monitoring to pair with overarching continuous calibration. 
 *NatNetClient class. Usage CalibHealthSystem [ClientIP] [ServerIP] [Optional: Multicast]
 *********************************************************************/

#include <inttypes.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

// stl
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>

#include "NatNetCAPI.h"
#include "CalibHealthSystem.h"
using namespace std;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32

#include <conio.h>
char ReadChar() {
    return static_cast<char>(_getch());
}
#endif

//Linux implementation
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
char ReadChar() {
  char buf = 0;
  termios old = {0};

  fflush(stdout);

  if (tcgetattr(0, &old) < 0) perror("tcsetattr()");

  old.c_lflag &= ~ICANON;
  old.c_lflag &= ~ECHO;
  old.c_cc[VMIN] = 1;
  old.c_cc[VTIME] = 0;

  if (tcsetattr(0, TCSANOW, &old) < 0) perror("tcsetattr ICANON");

  if (read(0, &buf, 1) < 0) perror("read()");

  old.c_lflag |= ICANON;
  old.c_lflag |= ECHO;

  if (tcsetattr(0, TCSADRAIN, &old) < 0) perror("tcsetattr ~ICANON");

  // printf( "%c\n", buf );

  return buf;
}
#endif

//Connection Variables
NatNetClient* g_pClient = NULL;
sNatNetClientConnectParams g_connectParams;
sServerDescription g_serverDesc;
sDataDescriptions* g_pDataDefs = nullptr;

float rbHealth = -1.0;

auto lastPrint = std::chrono::steady_clock::now();
auto lastDataDesc = std::chrono::steady_clock::now();
static bool printedLines = false;
bool isFirstFrame = true;

const int DATA_START_ROW = 11;
const int DIVIDER_ROW = 10;
const int INPUT_ROW = 9;

void PrintData(sFrameOfMocapData* data, NatNetClient* pClient);
static void NATNET_CALLCONV NatNetDataHandler(sFrameOfMocapData* data,
                                              void* pUserData);
int ConnectClient();
void ResetClient();
void InitDisplay();
bool UpdateDataDescriptions(bool printToConsole);
void ProcessUserInput(bool AcceptRawInput);
float DistanceCalc(std::vector<float>& initPoint, std::vector<float>& point);
static double QuaterionRotCalc(float x1, float y1, float z1, float w1,
                               QuaterionPoint* healthStandard);

std::vector<sAnchorMap>& cDataAccess::GetAnchors() { return mAnchors; }
std::vector<sCameraMap>& cDataAccess::GetCameras() { return mCameras; }
std::vector<sRigidBodyMap>& cDataAccess::GetRBs() { return mRBs; }

int main(int argc, char* argv[])
{ 
	ErrorCode ret = ErrorCode_OK;

	unsigned char ver[4];
	NatNet_GetVersion(ver);
    printf("\033[H\033[2J");
    printf("NatNet Calibration Health System (NatNet ver. %d.%d.%d.%d)\n", ver[0], ver[1], ver[2], ver[3] );
	//Connect
    g_pClient = new cNatNetClientWithStorage();
    g_pClient->SetFrameReceivedCallback(NatNetDataHandler, g_pClient);
    g_connectParams.localAddress = "127.0.0.1";
    g_connectParams.serverAddress = "127.0.0.1";
    g_connectParams.connectionType = ConnectionType_Multicast;
    if (argc > 4)
    {
      fprintf(stderr, "Usage: ./CalibHealthSystem <local> <server> <streaming_type>");
      return 1;
    }
    switch (argc)
    { 
        case 1:
            break;
        case 2:
          g_connectParams.localAddress = argv[1];
          break;
        case 3:
          g_connectParams.localAddress = argv[1];
          g_connectParams.serverAddress = argv[2];
          break;
        case 4:
          g_connectParams.localAddress = argv[1];
          g_connectParams.serverAddress = argv[2];
          if (argv[3] != NULL)
          {
            if (strcmp(argv[3], "Unicast") == 0 || strcmp(argv[3], "unicast") == 0) {
              g_connectParams.connectionType = ConnectionType_Unicast;
            } else {
              g_connectParams.connectionType = ConnectionType_Multicast;
            }
          }
          break;
    }
    std::string input;
    ret = g_pClient->Connect(g_connectParams);

	if (ret != ErrorCode_OK) {
      try {
        // Try Unicast Form
        g_connectParams.connectionType = ConnectionType_Unicast;
        ret = g_pClient->Connect(g_connectParams);
        printf("Multicast Failure: Connecting with Unicast...\n");
      }
      // Catch any other error
      catch (...) {
        // Connection failed
        printf("Unable to connect to server.  Error code: %d. Exiting.\n", ret);
        return 1;
      }
    }

    memset(&g_serverDesc, 0, sizeof(g_serverDesc));
    ret = g_pClient->GetServerDescription(&g_serverDesc);
    if (ret != ErrorCode_OK || !g_serverDesc.HostPresent)
    {
        printf("Unable to get server description. Error Code: %d.  Exiting.\n", ret);
        return 1;
    }
    else
    {
        printf("Connected : %s (ver. %d.%d.%d.%d)\n\n", g_serverDesc.szHostApp, g_serverDesc.HostAppVersion[0],
            g_serverDesc.HostAppVersion[1], g_serverDesc.HostAppVersion[2], g_serverDesc.HostAppVersion[3]);
    }
    ret = g_pClient->GetDataDescriptionList(&g_pDataDefs);
    if (ret != ErrorCode_OK || !g_pDataDefs) {
      printf(
          "Unable to retrieve data descriptions. Error Code: %d. Exiting. \n",
          ret);
      return 1;
    }
   
    // Construct UI 
    // Commands for basic program
    printf("Command List: \n\n");
    printf("q: Quit\n");
    printf("r: Reset Health Metrics (Edit Mode)\n");
    InitDisplay();
    ProcessUserInput(true); 
}

void cNatNetClientWithStorage::NatNetFrameCallback(sFrameOfMocapData* pFrame) {
  sGeneralMap genMap = cNatNetClientWithStorage::ObtainDataDescriptions(g_pDataDefs, pFrame);
  std::vector<sAnchorMap> anchorMap = genMap.anchorMap;
  std::vector<sCameraMap> camMap = genMap.camMap;
  std::vector<sRigidBodyMap> rbMap = genMap.rbMap;
  float meanError = meanMarkerError;
  auto currTime = std::chrono::steady_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(currTime - lastPrint);
  if (elapsed.count() >= 2)
  {
      PrintDataDescriptions(anchorMap, camMap, rbMap, meanMarkerError);
      lastPrint = currTime;
  }
  auto now = std::chrono::steady_clock::now();
  auto data_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastDataDesc);
  if (data_elapsed.count() >= 30)
  {
    ProcessMeanMarkerError(pFrame);
    ErrorCode ret = ErrorCode_OK;
    if (g_pDataDefs) 
    {
        NatNet_FreeDescriptions(g_pDataDefs);
        g_pDataDefs = nullptr;
    }
    ret = g_pClient->GetDataDescriptionList(&g_pDataDefs);
    lastDataDesc = now;
  }
  return;
};

void cNatNetClientWithStorage::HandleFrame(sFrameOfMocapData* data)
{
    NatNetFrameCallback(data);
}

//Alter this to ensure that client side gets to accept data as it chooses to, drops the rest of the frames. 
static void NATNET_CALLCONV NatNetDataHandler(sFrameOfMocapData* data, void* pUserData)
{
  auto* pClient = static_cast<cNatNetClientWithStorage*>(pUserData);
  if (!pClient)
  {
    return;
  }
  pClient->HandleFrame(data);
}

//Tolerance Comparison Helper Functions
bool cNatNetClientWithStorage::MarkerTolerance(float currMarkerCalib, float mainMarkerCalib)
{
  float mMarkerCalibDiff = abs(currMarkerCalib - mainMarkerCalib);
  return (mMarkerCalibDiff >= mMarkerToleranceDeviation);
}

bool cNatNetClientWithStorage::AnchorTolerance(float currAnchorCalib,
                                             float mainAnchorCalib) {
  float mAnchorCalibDiff = abs(currAnchorCalib - mainAnchorCalib);
  return (mAnchorCalibDiff >= mAnchorToleranceDeviation);
}

bool cNatNetClientWithStorage::CameraTolerance(float currCameraCalib,
                                             float mainCameraCalib) {
  float mCameraCalibDiff = abs(currCameraCalib - mainCameraCalib);
  return (mCameraCalibDiff >= mCameraToleranceDeviation);
}

bool cNatNetClientWithStorage::RigidBodyTolerance(float currRigidBodyCalib,
                                                float mainRigidBodyCalib) {
  float mRigidBodyCalibDiff = abs(currRigidBodyCalib - mainRigidBodyCalib);
  return (mRigidBodyCalibDiff >= mRigidBodyToleranceDeviation);
}

const float cNatNetClientWithStorage::GetAnchorToleranceDeviation()
{
  return mAnchorToleranceDeviation;
}

const float cNatNetClientWithStorage::GetCameraToleranceDeviation()
{
  return mCameraToleranceDeviation;
}

const float cNatNetClientWithStorage::GetRBToleranceDeviation()
{
  return mRigidBodyToleranceDeviation;
}

const float cNatNetClientWithStorage::GetMarkerToleranceDeviation()
{
  return mMarkerToleranceDeviation;
}

void cToleranceMetrics::SetAnchorTolerance(float anchorAverageDeviation)
{
	mainAnchorCalib = anchorAverageDeviation;
}
    
void cToleranceMetrics::SetCameraTolerance(float cameraAverageDeviation)
{
	mainCameraCalib = cameraAverageDeviation;
}

void cToleranceMetrics::SetMarkerTolerance(float markerAverageDeviation)
{
	mainMarkerCalib = markerAverageDeviation;
}

void cToleranceMetrics::SetRigidBodyTolerance(float rigidBodyAverageDeviation)
{
	mainRigidBodyCalib = rigidBodyAverageDeviation;
}

void cDataAccess::AddAnchor(sAnchorMap anchorMap)
{
    mAnchors.push_back(anchorMap);
}

void cDataAccess::AddCamera(sCameraMap camMap)
{
  mCameras.push_back(camMap);
}

void cDataAccess::AddRB(sRigidBodyMap rbMap) 
{ 
    mRBs.push_back(rbMap);
}

/*
* DistanceCalc
* Calculates the difference between two points
* Returns the distance as a float value. 
*/
float DistanceCalc(std::vector<float> &initPoint, std::vector<float>& p) {
  double fx = initPoint[0] - p[0];
  double fy = initPoint[1] - p[1];
  double fz = initPoint[2] - p[2];
  double distance = std::sqrt(fx * fx + fy * fy + fz * fz);
  return (float)distance;
}

/*
* QuaterionRotCalc
* Calculates the quaterion distance between two rotational values.
* While not included in the direct sample, this function is provided for integration into a UI
* Returns this diff as degrees
*/
static double QuaterionRotCalc(float x, float y, float z, float w, QuaterionPoint* q2)
{
    double n1 = std::sqrt(w * w + x * x + y * y + z * z);
    double n2 = std::sqrt(q2->qw * q2->qw + q2->qx * q2->qx + q2->qy * q2->qy + q2->qz * q2->qz);

    double w1 = w / n1, x1 = x / n1, y1 = y / n1, z1 = z / n1;
    double w2 = q2->qw / n2, x2 = q2->qx / n2, y2 = q2->qy / n2, z2 = q2->qz / n2;

    double dotProd = w1 * w2 + x1 * x2 + y1 * y2 + z1 * z2;
    if (dotProd > 1.0) dotProd = 1.0;
    if (dotProd < -1.0) dotProd = -1.0;

    double radians = 2.0 * acos(fabs(dotProd));
    double degrees = radians * (180.0 / M_PI);
    return degrees;
}

/*
* Process Camera Map
* Takes in Camera Data Description
* Updates its camera's relative position compared to when monitoring started
*/
void cNatNetClientWithStorage::ProcessCameraMap(sCameraDescription* pCamDesc) {
  std::string name = pCamDesc->strName;
  std::vector<sCameraMap>& cameras = progData.GetCameras();
  auto found = std::find_if(
      cameras.begin(), cameras.end(),
      [&](const sCameraMap& camera) { return camera.cameraName == name; });
  if (found != cameras.end()) {
    // Camera has already been found
    found->x = pCamDesc->x;
    found->y = pCamDesc->y;
    found->z = pCamDesc->z;
    found->qx = pCamDesc->qx;
    found->qy = pCamDesc->qy;
    found->qz = pCamDesc->qz;
    found->qw = pCamDesc->qw;
    QuaterionPoint* quatPoint = new QuaterionPoint;
    quatPoint->qx = found->qx;
    quatPoint->qy = found->qy;
    quatPoint->qz = found->qz;
    quatPoint->qw = found->qw;
    std::vector<float> initPoint = {found->x, found->y, found->z};
    std::vector<float> initRot = {found->qx, found->qy, found->qz, found->qw};
    if (!printedLines)
    {
      found->camPosHealthStandard = initPoint;
      found->camRotHealthStandard = initRot;
    }
    found->camPosDeviation =
        DistanceCalc(initPoint, found->camPosHealthStandard);
    found->camRotDeviation =
        QuaterionRotCalc(found->qx, found->qy, found->qz, found->qw, quatPoint);
  }
  else {
      if (cameras.size() <= MaxCams)
      {
          sCameraMap camMap;
          camMap.cameraName = pCamDesc->strName;
          camMap.x = pCamDesc->x;
          camMap.y = pCamDesc->y;
          camMap.z = pCamDesc->z;
          camMap.qx = pCamDesc->qx;
          camMap.qy = pCamDesc->qy;
          camMap.qz = pCamDesc->qz;
          camMap.qw = pCamDesc->qw;
          QuaterionPoint* quatPoint = new QuaterionPoint;
          quatPoint->qx = camMap.qx;
          quatPoint->qy = camMap.qy;
          quatPoint->qz = camMap.qz;
          quatPoint->qw = camMap.qw;
          camPosMetrics = {camMap.x, camMap.y, camMap.z};
          camRotMetrics = {camMap.qx, camMap.qy, camMap.qz, camMap.qw};
          camMap.camPosHealthStandard = camPosMetrics;
          camMap.camRotHealthStandard = camRotMetrics;
          camMap.camPosDeviation =
              1000 * DistanceCalc(camPosMetrics, camMap.camPosHealthStandard);
          camMap.camRotDeviation =
              QuaterionRotCalc(camMap.x, camMap.y, camMap.z, camMap.qw, quatPoint);
          progData.AddCamera(camMap);
      }
  }
}

/*
* ProcessAnchorMap
* Takes in a frame of data, and an anchor map
* Associates marker with closest passive marker distance that is not connected to an asset
* Should be noted that this assumes markers are far enough away from each other to not create major outliers
*/
void cNatNetClientWithStorage::ProcessAnchorMap(sAnchorDescription* pAnchor, sFrameOfMocapData* frame, std::vector<bool>markersUsed) {
  std::string name = pAnchor->szName;
  std::vector<sAnchorMap>& anchors = progData.GetAnchors();
  auto found = std::find_if(
      anchors.begin(), anchors.end(),
      [&](const sAnchorMap& anchor) { return anchor.anchorName == name; });
  if (found != anchors.end()) {
    // already exists
    found->xCoord = pAnchor->x;
    found->yCoord = pAnchor->y;
    found->zCoord = pAnchor->z;
    anchorMetrics = {found->xCoord, found->yCoord, found->zCoord};
    ProcessMarkerPoints(frame, &*found, markersUsed);
  } 
  else {
      if (anchors.size() <= MaxAnchors)
      {
          sAnchorMap anchorMap;
          anchorMap.anchorName = pAnchor->szName;
          anchorMap.xCoord = pAnchor->x;
          anchorMap.yCoord = pAnchor->y;
          anchorMap.zCoord = pAnchor->z;
          anchorMetrics = {anchorMap.xCoord, anchorMap.yCoord, anchorMap.zCoord};
          anchorMap.anchorHealthStandard = anchorMetrics;
          anchorMap.anchorDeviation =
              DistanceCalc(anchorMetrics, anchorMap.anchorHealthStandard);
          anchorMap.markerActualArray = anchorMetrics;
          progData.AddAnchor(anchorMap);
      }
  }
}

/*
* ProcessRBMap
* Takes in a Rigid Body description, and frame of data
*/
void cNatNetClientWithStorage::ProcessRBMap(sRigidBodyDescription* pRigidBody,
                                            sFrameOfMocapData* frameData,
                                            int iterRbs) {
  std::string name = pRigidBody->szName;
  std::vector<sRigidBodyMap>& rigidBodies = progData.GetRBs();
  auto found = std::find_if(
      rigidBodies.begin(), rigidBodies.end(),
      [&](const sRigidBodyMap& rigidBody) { return rigidBody.rbName == name; });
  if (found != rigidBodies.end()) {
      if (!printedLines)
      {
        found->rbHealthStandard = frameData->RigidBodies[iterRbs].MeanError;
      }
    found->residualError = frameData->RigidBodies[iterRbs].MeanError;
    found->rbDeviation = abs(found->rbHealthStandard - found->residualError);
  } 
  else {
      if (rigidBodies.size() <= MaxRBs)
      {
        sRigidBodyMap rbMap;
        rbMap.rbName = pRigidBody->szName;
        if (rbHealth < 0) {
          rbMap.rbHealthStandard = frameData->RigidBodies[iterRbs].MeanError;
        }
        rbMap.residualError = frameData->RigidBodies[iterRbs].MeanError;
        rbMap.rbDeviation =
            1000 * abs(rbMap.rbHealthStandard - rbMap.residualError);
        progData.AddRB(rbMap);
      }
  }
}

/*
* ProcessMeanMarkerError()
* Takes in a frame of data, processes every labeled marker in the scene
* Updates the mean count for printing
*/

void cNatNetClientWithStorage::ProcessMeanMarkerError(sFrameOfMocapData* frame)
{
  int totalMarkers = frame->nLabeledMarkers;
  float meanCount = 0.0;
  for (int iMarker = 0; iMarker < totalMarkers; iMarker++)
  {
    sMarker marker = frame->LabeledMarkers[iMarker];
    meanCount += marker.residual;
  }
  meanMarkerError = (meanCount / totalMarkers) * 1000;
}

/*
* Process Marker Points
* Takes in a frame of data, and an anchor map. 
* Ensures that marker can be associated with anchor marker, as anchor marker values do not stream through
*/
void cNatNetClientWithStorage::ProcessMarkerPoints(sFrameOfMocapData* frame, sAnchorMap* pAnchorMap, std::vector<bool>& markerUsed)
{
    int nUnattachedMarkers = frame->nOtherMarkers;
    float prevDistance = (std::numeric_limits<float>::max)();
    int bestMarker = -1;
    std::vector<float> anchorHealthStandard = pAnchorMap->anchorHealthStandard;
    for (int i = 0; i < nUnattachedMarkers; i++)
    {
      if (markerUsed[i]) 
          continue;
      float xVal = frame->OtherMarkers[i][0];
      float yVal = frame->OtherMarkers[i][1];
      float zVal = frame->OtherMarkers[i][2];
      if (xVal == 0.0f && yVal == 0.0f && zVal == 0.0f) continue;
      std::vector<float> markerVals = {xVal, yVal, zVal};
      float distance = DistanceCalc(markerVals, anchorHealthStandard);
      if (distance < prevDistance)
      {
        prevDistance = distance;
        bestMarker = i;
      }
    }
    if (bestMarker >= 0) {
      markerUsed[bestMarker] = true;

      float xVal = frame->OtherMarkers[bestMarker][0];
      float yVal = frame->OtherMarkers[bestMarker][1];
      float zVal = frame->OtherMarkers[bestMarker][2];
      std::vector<float> markerVals = {xVal, yVal, zVal};
      pAnchorMap->markerActualArray = markerVals;
      if (!printedLines) {
        pAnchorMap->anchorHealthStandard = markerVals;
      }
    }
    float residual = DistanceCalc(pAnchorMap->markerActualArray, anchorHealthStandard);
    pAnchorMap->anchorDeviation = residual * 1000;
 };       

sGeneralMap cNatNetClientWithStorage::ObtainDataDescriptions(sDataDescriptions* pDataDefs, sFrameOfMocapData* frameData) {
  cDataAccess data = this->progData;
  int iterAnchors = data.GetAnchors().size();
  int iterCameras = data.GetCameras().size();
  int iterRbs = data.GetRBs().size();
  std::vector<bool> markersUsed(frameData->nOtherMarkers, false); 
  for (int i = 0; i < pDataDefs->nDataDescriptions; i++) 
  {
    if (pDataDefs->arrDataDescriptions[i].type == Descriptor_Anchor &&
        iterAnchors < MaxAnchors) {
      sAnchorDescription* pAnchor =
          pDataDefs->arrDataDescriptions[i].Data.AnchorDescription;
      if (pAnchor->szName) {
          ProcessAnchorMap(pAnchor, frameData, markersUsed);
          if (data.GetAnchors().size() > iterAnchors)
          {
            iterAnchors++;
          }
      }
    } 
    else if (pDataDefs->arrDataDescriptions[i].type == Descriptor_RigidBody && iterRbs < MaxRBs) 
    {
      sRigidBodyDescription* pRigidBody = pDataDefs->arrDataDescriptions[i].Data.RigidBodyDescription;
      if (pRigidBody->szName)
      {
        ProcessRBMap(pRigidBody, frameData, iterRbs);
        if (data.GetRBs().size() > iterRbs)
        {
          iterRbs++;
        }
      }
    } 
    else if (pDataDefs->arrDataDescriptions[i].type == Descriptor_Camera &&
               iterCameras < MaxCams)
    {
      sCameraDescription* pCamDesc = pDataDefs->arrDataDescriptions[i].Data.CameraDescription;
      if (pCamDesc->strName) {
        ProcessCameraMap(pCamDesc);
        if (data.GetCameras().size() > iterCameras)
        {
          iterCameras++;
        }
      }
    }
  }
  sGeneralMap genMap = {progData.GetAnchors(), progData.GetCameras(), progData.GetRBs()};
  return genMap;
}

int cNatNetClientWithStorage::PrintAnchorDescriptions(std::vector<sAnchorMap> anchorMap, int row)
{
    float anchorToleranceDeviation = GetAnchorToleranceDeviation();
    printf("\033[%d;1HAnchor Markers | Distance (mm)", row);
    row++;
    for (int anchorNum = 0; anchorNum < anchorMap.size(); anchorNum++) 
    {
        if (printedLines && (double) anchorMap[anchorNum].anchorDeviation > anchorToleranceDeviation) 
          {
            printf("\033[%d;1H\033[K%s | \033[31m%.2f\033[0m", row, anchorMap[anchorNum].anchorName.c_str(),(double)anchorMap[anchorNum].anchorDeviation);
          }
        else
          {
            printf("\033[%d;1H\033[K%s | %.2f", row, anchorMap[anchorNum].anchorName.c_str(), (double)anchorMap[anchorNum].anchorDeviation);
          }
    fflush(stdout);
    row++;
    }
    return row;
}

int cNatNetClientWithStorage::PrintCameraDescriptions(std::vector<sCameraMap> camMap, int row) 
{
    float cameraToleranceDeviation = GetCameraToleranceDeviation();
    row++;
    printf("\033[%d;1HCameras", row);
    row++;
    for (int camNum = 0; camNum < camMap.size(); camNum++)
    {
        if (printedLines && (double)camMap[camNum].camPosDeviation > cameraToleranceDeviation)
        {
            printf("\033[%d;1H\033[K%s | \033[31m%.2f\033[0m", row, camMap[camNum].cameraName.c_str(),
                  (double)camMap[camNum].camPosDeviation);
        }
        else
        {
            printf("\033[%d;1H\033[K%s | %.2f", row, camMap[camNum].cameraName.c_str(),
                  (double)camMap[camNum].camPosDeviation);
        }
        row++;
    }
    return row;
}

int cNatNetClientWithStorage::PrintRBDescriptions(std::vector<sRigidBodyMap> rbMap, int row)
{
  float rbToleranceDeviation = GetRBToleranceDeviation();
  row++;
  printf("\033[%d;1HRigid Bodies", row);
  row++;

  for (int rbNum = 0; rbNum < rbMap.size(); rbNum++) {
    if (printedLines &&
        (double)rbMap[rbNum].rbDeviation > rbToleranceDeviation) {
      printf("\033[%d;1H\033[K%s | \033[31m%.2f\033[0m", row,
             rbMap[rbNum].rbName.c_str(), (double)rbMap[rbNum].rbDeviation);
    } else {
      printf("\033[%d;1H\033[K%s | %.2f", row, rbMap[rbNum].rbName.c_str(),
             (double)rbMap[rbNum].rbDeviation);
    }
    fflush(stdout);
    row++;
  }
  return row;
}

int cNatNetClientWithStorage::PrintMeanMarkerError(float meanMarkerError, int row) {
  row++;
  printf("\033[%d;1HMean Marker Error: ", row); 
  if (meanMarkerError > mMeanMarkerErrorToleranceDeviation) 
  {
    printf("\033[31m%.2f\033[0m", meanMarkerError);
  }
  else
  {
    printf("%.2f", meanMarkerError);
  }
  fflush(stdout);
  return row;
}

void cNatNetClientWithStorage::PrintDataDescriptions(
    std::vector<sAnchorMap> anchorMap, std::vector<sCameraMap> camMap,
    std::vector<sRigidBodyMap> rbMap, float meanMarkerError) {
  Terminal::MoveTo(DATA_START_ROW, 1);
  Terminal::ClearToEnd();
  int row = DATA_START_ROW;
  if (anchorMap.size() > 0) {
    row = PrintAnchorDescriptions(anchorMap, row);
  }
  if (camMap.size() > 0) {
    row = PrintCameraDescriptions(camMap, row);
  }
  if (rbMap.size() > 0) {
    row = PrintRBDescriptions(rbMap, row);
  }
  row = PrintMeanMarkerError(meanMarkerError, row);
  if (!printedLines)
  {
    printedLines = true;
  }
  Terminal::MoveTo(INPUT_ROW, 3);
  fflush(stdout);
}

void InitDisplay() {
  Terminal::Init();
  Terminal::MoveTo(DIVIDER_ROW, 1);
  Terminal::ClearToEnd();
  const std::string divider = "----------------------------------------";
  Terminal::Print(divider);
}

void ProcessUserInput(bool rawInputEnabled) {
  std::string input = "";
  Terminal::MoveTo(INPUT_ROW, 1);
  printf("> ");
  

  while (true) {
    char c = ReadChar();
      if (c == '\r' || c == '\n')
      {
        if (input == "q")
          {
            printf("\033[10;1H");
            printf("\033[J");
            break;
          }
        else if (input == "r")
          {
            Terminal::ClearToEnd();
            Terminal::MoveTo(DATA_START_ROW, 1);
            printedLines = false;
            input.clear();
            Terminal::MoveTo(INPUT_ROW, 1);
            Terminal::ClearLine();
            Terminal::Print(">");
            Terminal::MoveTo(INPUT_ROW, 3);
            continue;
          }

      input.clear();
      Terminal::MoveTo(INPUT_ROW, 1);
      Terminal::ClearLine();
      Terminal::Print(">");
      fflush(stdout);
    } else {
      input.push_back(c);
      putchar(c);
      fflush(stdout);
    }
  }
  printf("\n");
}
