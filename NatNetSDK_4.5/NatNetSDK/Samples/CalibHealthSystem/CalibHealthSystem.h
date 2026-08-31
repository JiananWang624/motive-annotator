
#include <vector>
#include <string>
#include <NatNetClient.h>
#include <NatNetTypes.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <cstdio>
#endif

 struct sRigidBodyMap {
  std::string rbName;
  float residualError;
  float rbDeviation;
  float rbHealthStandard;
};

 struct sAnchorMap {
  std::string anchorName;
   float xCoord;
   float yCoord;
   float zCoord;
  std::vector<float> markerActualArray;
  std::vector<float> anchorHealthStandard;
  float anchorDeviation;
 };

 struct sCameraMap {
   std::string cameraName;
   std::vector<float> camPosHealthStandard;
   std::vector<float> camRotHealthStandard;
   float x;
   float y;
   float z;
   float qx;
   float qy;
   float qz;
   float qw;
   float camPosDeviation;
   float camRotDeviation;
 };

 struct sKNNPoint
 {
   float x;
   float y;
   float z;
   float distance;
 };

 struct sMarkerMap
 {
   std::string markerName;
   int markerID;
   float x;
   float y;
   float z;
   float markerResidual;
 };

 struct sGeneralMap
 {
   std::vector<sAnchorMap> anchorMap;
   std::vector<sCameraMap> camMap;
   std::vector<sRigidBodyMap> rbMap;
   std::vector<sMarkerMap> markerMap;
 };

 struct QuaterionPoint
 {
   double qx, qy, qz, qw;
 };

  class cDataAccess {
  public:
   void AddAnchor(sAnchorMap anchorMap);
   void AddRB(sRigidBodyMap rbMap);
   void AddCamera(sCameraMap camMap);
   std::vector<sAnchorMap>& GetAnchors();
   std::vector<sCameraMap>& GetCameras();
   std::vector<sRigidBodyMap>& GetRBs();

  private:
   std::vector<sRigidBodyMap> mRBs;
   std::vector<sCameraMap> mCameras;
   std::vector<sAnchorMap> mAnchors;
   std::vector<sMarkerMap> mMarkers;
 };

 class cNatNetClientWithStorage : public NatNetClient
 {
  public:
   cNatNetClientWithStorage() = default;
   ~cNatNetClientWithStorage() = default;
   static void NATNET_CALLCONV NatNetDataHandler(sFrameOfMocapData* data, void* pUserData); 
   void HandleFrame(sFrameOfMocapData* data);
   sGeneralMap ObtainDataDescriptions(sDataDescriptions* pDataDefs,
                                      sFrameOfMocapData* pFrameData);
   void NatNetFrameCallback(sFrameOfMocapData* pFrame);
   const float GetAnchorToleranceDeviation();
   const float GetCameraToleranceDeviation();
   const float GetRBToleranceDeviation();
   const float GetMarkerToleranceDeviation();

  private:
   //flags
   bool MarkerTolerance(float currMarkerTolerance, float mainMarkerTolerance);
   bool AnchorTolerance(float currAnchorTolerance, float mainAnchorTolerance);
   bool CameraTolerance(float currCameraTolerance, float mainCameraTolerance);
   bool RigidBodyTolerance(float currRigidBodyTolerance,
                           float mainRigidBodyTolerance);
   void ProcessCameraMap(sCameraDescription* camDesc);
   void ProcessRBMap(sRigidBodyDescription* rbDesc, sFrameOfMocapData* data, int iterRBs);
   void ProcessAnchorMap(sAnchorDescription* anchorDesc, sFrameOfMocapData* data,  std::vector<bool>markersUsed);
   void ProcessMarkerPoints(sFrameOfMocapData* frame, sAnchorMap* anchorMap, std::vector<bool>& markerUsed);
   void ProcessMeanMarkerError(sFrameOfMocapData* frame);
   int PrintAnchorDescriptions(std::vector<sAnchorMap> anchorMap, int row);
   int PrintCameraDescriptions(std::vector<sCameraMap> camMap, int row);
   int PrintRBDescriptions(std::vector<sRigidBodyMap> rbMap, int row);
   int PrintMeanMarkerError(float meanMarkerError, int row);
   void PrintDataDescriptions(std::vector<sAnchorMap> anchorMap,
                              std::vector<sCameraMap> camMap,
                              std::vector<sRigidBodyMap> rbMap,
                              float meanMarkerError);

   cDataAccess progData;
   std::vector<sKNNPoint> markerPoints;
   float mMarkerToleranceDeviation = 0.6;
   float mAnchorToleranceDeviation = 0.6;
   float mCameraToleranceDeviation = 0.6;
   float mRigidBodyToleranceDeviation = 0.6;
   float mMeanMarkerErrorToleranceDeviation = 0.6;

   std::vector<float> anchorMetrics;
   std::vector<float> camPosMetrics;
   std::vector<float> camRotMetrics;

   int MaxCams = 10;
   int MaxAnchors = 10;
   int MaxRBs = 10;
   float meanMarkerError = 0.0;
 };


class cToleranceMetrics : public cDataAccess {
 public:
  void SetCameraTolerance(float camTolerance);
  void SetRigidBodyTolerance(float rbTolerance);
  void SetAnchorTolerance(float anchorTolerance);
  void SetMarkerTolerance(float markerTolerance);
 private:

	//Main tolerance sampling variables
	float mainMarkerCalib;
    float mainAnchorCalib;
    float mainCameraCalib;
    float mainRigidBodyCalib;

	float mCurrMarkerCalib;
    float mCurrAnchorCalib;
    float mCurrCameraCalib;
    float mCurrRigidBodyCalib;
};

namespace Terminal
{
    inline void Init()
    {
    #ifdef _WIN32
        HANDLE mSTDHandleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        GetConsoleMode(mSTDHandleOutput, &mode);
        SetConsoleMode(mSTDHandleOutput, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    #endif
    }

    inline void MoveTo(int row, int col)
    {
    #ifdef _WIN32
      HANDLE mSTDHandleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      COORD cursPos = {(SHORT)(col - 1), (SHORT)(row - 1)};
      SetConsoleCursorPosition(mSTDHandleOutput, cursPos);
    #else
        printf("\033[%d;%dH", row, col);
    #endif
    }

    inline void ClearToEnd()
    {
    #ifdef _WIN32
        HANDLE mSTDHandleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info;
        GetConsoleScreenBufferInfo(mSTDHandleOutput, &info);
        COORD cursPos = {0, info.dwCursorPosition.Y};
        DWORD written;
        FillConsoleOutputCharacter(mSTDHandleOutput, ' ', info.dwSize.X, cursPos, &written);
        SetConsoleCursorPosition(mSTDHandleOutput, cursPos);
    #else
      printf("\r\033[J");
    #endif
    }

    inline void ClearLine() {
    #ifdef _WIN32
      HANDLE mSTDHandleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      CONSOLE_SCREEN_BUFFER_INFO info;
      GetConsoleScreenBufferInfo(mSTDHandleOutput, &info);
      COORD cur = {0, info.dwCursorPosition.Y};
      DWORD written;
      FillConsoleOutputCharacter(mSTDHandleOutput, ' ', info.dwSize.X, cur, &written);
      SetConsoleCursorPosition(mSTDHandleOutput, cur);
    #else
      printf("\r\033[K");
    #endif
    }

    inline void Print(const std::string& text)
    {
        printf("%s", text.c_str());
        fflush(stdout);
    }
}