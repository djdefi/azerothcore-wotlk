/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _PATH_GENERATOR_H
#define _PATH_GENERATOR_H

#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"
#include "MMapMgr.h"
#include "MapDefines.h"
#include "MoveSplineInitArgs.h"
#include "SharedDefines.h"
#include <G3D/Vector3.h>

class Unit;
class WorldObject;

// 74*4.0f=296y number_of_points*interval = max_path_len
// this is way more than actual evade range
// I think we can safely cut those down even more
#ifdef MOD_PLAYERBOTS
// Bots travel long-distance to quests; the default 74-poly cap forces
// repeated re-pathfinding mid-route and produces partial paths short of
// the destination. 148 covers most quest movements end-to-end.
#define MAX_PATH_LENGTH         148
#define MAX_POINT_PATH_LENGTH   148
#else
#define MAX_PATH_LENGTH         74
#define MAX_POINT_PATH_LENGTH   74
#endif

#define SMOOTH_PATH_STEP_SIZE   4.0f
#define SMOOTH_PATH_SLOP        0.3f
#define DISALLOW_TIME_AFTER_FAIL    3 // secs
#define VERTEX_SIZE       3
#define INVALID_POLYREF   0

enum PathType
{
    PATHFIND_BLANK             = 0x00,   // path not built yet
    PATHFIND_NORMAL            = 0x01,   // normal path
    PATHFIND_SHORTCUT          = 0x02,   // travel through obstacles, terrain, air, etc (old behavior)
    PATHFIND_INCOMPLETE        = 0x04,   // we have partial path to follow - getting closer to target
    PATHFIND_NOPATH            = 0x08,   // no valid path at all or error in generating one
    PATHFIND_NOT_USING_PATH    = 0x10,   // used when we are either flying/swiming or on map w/o mmaps
    PATHFIND_SHORT             = 0x20,   // path is longer or equal to its limited path length
    PATHFIND_FARFROMPOLY_START = 0x40,   // start position is far from the mmap poligon
    PATHFIND_FARFROMPOLY_END   = 0x80,   // end positions is far from the mmap poligon
    PATHFIND_FARFROMPOLY       = PATHFIND_FARFROMPOLY_START | PATHFIND_FARFROMPOLY_END, // start or end positions are far from the mmap poligon
};

// Optional caller-owned evidence from one original calculation, not a replay or a path-safety certificate.
struct PathQueryDiagnostics
{
    enum class Check : uint8
    {
        NotEvaluated, No, Yes
    };
    enum class Lookup : uint8
    {
        NotEvaluated, CachedCorridor, SmallExtent, TallExtent, NotFound
    };
    enum class Shortcut : uint8
    {
        None, Prerequisite, MissingPolygon, FarFromPolygon, ForcedDestination
    };
    enum class CorridorMode : uint8
    {
        NotEvaluated, SamePolygon, Reused, Extended, FindPath, Raycast
    };
    enum class PointMode : uint8
    {
        NotEvaluated, Smooth, Straight, Raycast
    };

    struct Tile
    {
        Check Present = Check::NotEvaluated;
        int X = -1;
        int Y = -1;
    };

    struct Polygon
    {
        Lookup Source = Lookup::NotEvaluated;
        dtPolyRef Ref = INVALID_POLYREF;
        // Valid only after a lookup; FLT_MAX with NotFound is the existing missing-polygon sentinel.
        float Distance = 0.0f;
        uint32 CachedPolyCount = 0;
        Check SmallSearchSucceeded = Check::NotEvaluated;
        Check TallSearchSucceeded = Check::NotEvaluated;
        dtStatus SmallStatus = 0;
        dtStatus TallStatus = 0;
    };

    struct Corridor
    {
        CorridorMode Mode = CorridorMode::NotEvaluated;
        bool StatusAvailable = false;
        dtStatus Status = 0;
        // Counts/last ref are captured before any subsequent shortcut clears the corridor.
        uint32 QueryPolyCount = 0;
        uint32 PolyCount = 0;
        dtPolyRef LastPoly = INVALID_POLYREF;
        Check ReachesEndPolygon = Check::NotEvaluated;
        Check RaycastClear = Check::NotEvaluated;
        float RaycastHit = 0.0f;
    };

    struct Points
    {
        PointMode Mode = PointMode::NotEvaluated;
        bool StatusAvailable = false;
        dtStatus Status = 0;
        // Producer count, before close-target append, normalization or fallback; see ResultPointCount.
        uint32 Count = 0;
        Check SlopeLimited = Check::NotEvaluated;
        Check LimitReached = Check::NotEvaluated;
    };

    G3D::Vector3 Start{0.0f, 0.0f, 0.0f};
    G3D::Vector3 End{0.0f, 0.0f, 0.0f};
    uint32 MapId = 0;
    uint32 InstanceId = 0;
    uint32 PhaseMask = 0;
    bool ExplicitStart = false;
    bool ForceDestination = false;
    bool UseRaycast = false;
    bool UseStraightPath = false;
    bool SlopeCheck = false;
    uint32 PointLimit = 0;
    uint16 EntryIncludeFlags = 0;
    uint16 EntryExcludeFlags = 0;
    bool FilterUpdated = false;
    uint16 UsedIncludeFlags = 0;
    uint16 UsedExcludeFlags = 0;
    Check CoordinatesValid = Check::NotEvaluated;
    Check HasNavMesh = Check::NotEvaluated;
    Check HasNavMeshQuery = Check::NotEvaluated;
    Check IgnorePathfinding = Check::NotEvaluated;
    Tile StartTile;
    Tile EndTile;
    Polygon StartPolygon;
    Polygon EndPolygon;
    Check StartFar = Check::NotEvaluated;
    Check EndFar = Check::NotEvaluated;
    Corridor CorridorStage;
    Points PointStage;
    // Identifies the producer of NORMAL|NOT_USING_PATH, not the inferred reason for every partial/failure.
    Shortcut ShortcutReason = Shortcut::None;
    // False for invalid-coordinate attempts: the generator's previous path/type are deliberately left unchanged.
    bool ResultUpdated = false;
    PathType ResultType = PATHFIND_BLANK;
    G3D::Vector3 ActualEnd{0.0f, 0.0f, 0.0f};
    uint32 ResultPointCount = 0;
};

class PathGenerator
{
    public:
        explicit PathGenerator(WorldObject const* owner);
        ~PathGenerator();

        // Calculate the path from owner to given destination
        // return: true if new path was calculated, false otherwise (no change needed)
        bool CalculatePath(float destX, float destY, float destZ, bool forceDest = false);
        bool CalculatePath(float x, float y, float z, float destX, float destY, float destZ, bool forceDest);
        // Reset and populate only this caller-owned sink during the synchronous original query.
        // Raw Start/End precede normalization. NotEvaluated preserves short-circuiting; no extra probes are run.
        // Status fields require their availability marker; no sink is retained and existing overloads are unchanged.
        bool CalculatePath(float destX, float destY, float destZ, bool forceDest, PathQueryDiagnostics& diagnostics);
        bool CalculatePath(float x, float y, float z, float destX, float destY, float destZ, bool forceDest,
            PathQueryDiagnostics& diagnostics);
        [[nodiscard]] bool IsInvalidDestinationZ(Unit const* target) const;
        [[nodiscard]] bool IsWalkableClimb(float const* v1, float const* v2) const;
        [[nodiscard]] bool IsWalkableClimb(float x, float y, float z, float destX, float destY, float destZ) const;
        [[nodiscard]] static bool IsWalkableClimb(float x, float y, float z, float destX, float destY, float destZ, float sourceHeight);
        [[nodiscard]] bool IsWaterPath(Movement::PointsArray pathPoints) const;
        [[nodiscard]] bool IsSwimmableSegment(float const* v1, float const* v2, bool checkSwim = true) const;
        [[nodiscard]] bool IsSwimmableSegment(float x, float y, float z, float destX, float destY, float destZ, bool checkSwim = true) const;
        [[nodiscard]] static float GetRequiredHeightToClimb(float x, float y, float z, float destX, float destY, float destZ, float sourceHeight);

        // option setters - use optional

        // when set, it skips paths with too high slopes (doesn't work with StraightPath enabled)
        void SetSlopeCheck(bool checkSlope) { _slopeCheck = checkSlope; }
        void SetUseStraightPath(bool useStraightPath) { _useStraightPath = useStraightPath; }
        void SetPathLengthLimit(float distance) { _pointPathLimit = std::min<uint32>(uint32(distance/SMOOTH_PATH_STEP_SIZE), MAX_POINT_PATH_LENGTH); }
        void SetUseRaycast(bool useRaycast) { _useRaycast = useRaycast; }
        // Adjust per-terrain Detour traversal cost on the active query
        // filter. Persists across CalculatePath calls until overwritten.
        void SetNavTerrainCost(NavTerrain terrain, float cost)
        {
            _filter.setAreaCost(static_cast<uint8>(terrain), cost);
        }
        // Replace the active filter's exclude bitmask. Caller may pass
        // a single NavTerrain or an OR'd combination (NavTerrain values
        // implicitly convert through uint16).
        void SetExcludeFlags(uint16 flags) { _filter.setExcludeFlags(flags); }

        // result getters
        [[nodiscard]] G3D::Vector3 const& GetStartPosition() const { return _startPosition; }
        [[nodiscard]] G3D::Vector3 const& GetEndPosition() const { return _endPosition; }
        [[nodiscard]] G3D::Vector3 const& GetActualEndPosition() const { return _actualEndPosition; }

        [[nodiscard]] Movement::PointsArray const& GetPath() const { return _pathPoints; }

        [[nodiscard]] PathType GetPathType() const { return _type; }

        // shortens the path until the destination is the specified distance from the target point
        void ShortenPathUntilDist(G3D::Vector3 const& point, float dist);

        [[nodiscard]] float getPathLength() const
        {
            float len = 0.0f;
            float dx, dy, dz;
            uint32 size = _pathPoints.size();
            if (size)
            {
                dx = _pathPoints[0].x - _startPosition.x;
                dy = _pathPoints[0].y - _startPosition.y;
                dz = _pathPoints[0].z - _startPosition.z;
                len += std::sqrt( dx * dx + dy * dy + dz * dz );
            }
            else
            {
                return len;
            }

            for (uint32 i = 1; i < size; ++i)
            {
                dx = _pathPoints[i].x - _pathPoints[i - 1].x;
                dy = _pathPoints[i].y - _pathPoints[i - 1].y;
                dz = _pathPoints[i].z - _pathPoints[i - 1].z;
                len += std::sqrt( dx * dx + dy * dy + dz * dz );
            }
            return len;
        }

        void Clear()
        {
            _polyLength = 0;
            _pathPoints.clear();
        }

    private:
        bool CalculatePathImpl(float x, float y, float z, float destX, float destY, float destZ, bool forceDest,
            bool explicitStart, PathQueryDiagnostics* diagnostics);
        dtPolyRef _pathPolyRefs[MAX_PATH_LENGTH];   // array of detour polygon references
        uint32 _polyLength;                         // number of polygons in the path

        Movement::PointsArray _pathPoints;  // our actual (x,y,z) path to the target
        PathType _type;                     // tells what kind of path this is

        bool _useStraightPath;  // type of path will be generated (do not use it for movement paths)
        bool _forceDestination; // when set, we will always arrive at given point
        bool _slopeCheck;       // when set, it skips paths with too high slopes (doesn't work with _useStraightPath)
        uint32 _pointPathLimit; // limit point path size; min(this, MAX_POINT_PATH_LENGTH)
        bool _useRaycast;       // use raycast if true for a straight line path

        G3D::Vector3 _startPosition;        // {x, y, z} of current location
        G3D::Vector3 _endPosition;          // {x, y, z} of the destination
        G3D::Vector3 _actualEndPosition;    // {x, y, z} of the closest possible point to given destination

        WorldObject const* const _source;       // the object that is moving
        dtNavMesh const* _navMesh;              // the nav mesh
        dtNavMeshQuery const* _navMeshQuery;    // the nav mesh query used to find the path

        dtQueryFilterExt _filter;  // use single filter for all movements, update it when needed

        void SetStartPosition(G3D::Vector3 const& point) { _startPosition = point; }
        void SetEndPosition(G3D::Vector3 const& point) { _actualEndPosition = point; _endPosition = point; }
        void SetActualEndPosition(G3D::Vector3 const& point) { _actualEndPosition = point; }
        void NormalizePath();

        [[nodiscard]] bool InRange(G3D::Vector3 const& p1, G3D::Vector3 const& p2, float r, float h) const;
        [[nodiscard]] float Dist3DSqr(G3D::Vector3 const& p1, G3D::Vector3 const& p2) const;
        bool InRangeYZX(float const* v1, float const* v2, float r, float h) const;

        dtPolyRef GetPathPolyByPosition(dtPolyRef const* polyPath, uint32 polyPathSize, float const* Point, float* Distance = nullptr) const;
        dtPolyRef GetPolyByLocation(float const* Point, float* Distance,
            PathQueryDiagnostics::Polygon* diagnostics) const;
        [[nodiscard]] bool HaveTile(G3D::Vector3 const& p, PathQueryDiagnostics::Tile* diagnostics) const;

        void BuildPolyPath(G3D::Vector3 const& startPos, G3D::Vector3 const& endPos,
            PathQueryDiagnostics* diagnostics);
        void BuildPointPath(float const* startPoint, float const* endPoint, PathQueryDiagnostics* diagnostics);
        void BuildShortcut();

        [[nodiscard]] NavTerrain GetNavTerrain(float x, float y, float z) const;
        void CreateFilter();
        void UpdateFilter();

        // smooth path aux functions
        uint32 FixupCorridor(dtPolyRef* path, uint32 npath, uint32 maxPath, dtPolyRef const* visited, uint32 nvisited);
        bool GetSteerTarget(float const* startPos, float const* endPos, float minTargetDist, dtPolyRef const* path, uint32 pathSize, float* steerPos,
                            unsigned char& steerPosFlag, dtPolyRef& steerPosRef);
        dtStatus FindSmoothPath(float const* startPos, float const* endPos,
                              dtPolyRef const* polyPath, uint32 polyPathSize,
                              float* smoothPath, int* smoothPathSize, uint32 smoothPathMaxSize);

        void AddFarFromPolyFlags(bool startFarFromPoly, bool endFarFromPoly);
};

#endif
