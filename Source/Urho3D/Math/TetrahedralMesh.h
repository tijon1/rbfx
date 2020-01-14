//
// Copyright (c) 2008-2019 the Urho3D project.
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

/// \file

#pragma once

#include "../Math/BoundingBox.h"
#include "../Math/Matrix3x4.h"
#include "../Math/Vector3.h"

#include <EASTL/span.h>

#include <cmath>

namespace Urho3D
{

class Archive;

/// 3-vector with double precision.
struct HighPrecisionVector3
{
    /// Construct default.
    HighPrecisionVector3() = default;

    /// Construct from Vector3.
    explicit HighPrecisionVector3(const Vector3& vec) : data_{ vec.x_, vec.y_, vec.z_ } {}

    /// Convert to Vector3.
    explicit operator Vector3() const
    {
        return {
            static_cast<float>(data_[0]),
            static_cast<float>(data_[1]),
            static_cast<float>(data_[2])
        };
    }

    /// Dot product with another vector.
    double DotProduct(const HighPrecisionVector3& rhs) const
    {
        double result{};
        for (unsigned i = 0; i < 3; ++i)
            result += data_[i] * rhs.data_[i];
        return result;
    }

    /// Cross product with another vector.
    HighPrecisionVector3 CrossProduct(const HighPrecisionVector3& rhs) const
    {
        HighPrecisionVector3 result;
        result.data_[0] = data_[1] * rhs.data_[2] - data_[2] * rhs.data_[1];
        result.data_[1] = data_[2] * rhs.data_[0] - data_[0] * rhs.data_[2];
        result.data_[2] = data_[0] * rhs.data_[1] - data_[1] * rhs.data_[0];
        return result;
    }

    /// Return squared length of the vector.
    double LengthSquared() const { return DotProduct(*this); }

    /// Add another vector.
    HighPrecisionVector3 operator +(const HighPrecisionVector3& rhs) const
    {
        HighPrecisionVector3 result;
        for (unsigned i = 0; i < 3; ++i)
            result.data_[i] = data_[i] + rhs.data_[i];
        return result;
    }

    /// Subtract another vector.
    HighPrecisionVector3 operator -(const HighPrecisionVector3& rhs) const
    {
        HighPrecisionVector3 result;
        for (unsigned i = 0; i < 3; ++i)
            result.data_[i] = data_[i] - rhs.data_[i];
        return result;
    }

    /// Multiply with scalar.
    HighPrecisionVector3 operator *(double rhs) const
    {
        HighPrecisionVector3 result;
        for (unsigned i = 0; i < 3; ++i)
            result.data_[i] = data_[i] * rhs;
        return result;
    }

    /// Components.
    double data_[3]{};
};

/// Sphere with double precision components.
struct HighPrecisionSphere
{
    /// Center.
    HighPrecisionVector3 center_;
    /// Radius.
    double radius_{};

    /// Return signed distance from position to the sphere.
    double Distance(const Vector3& position) const
    {
        const auto doublePosition = static_cast<HighPrecisionVector3>(position);
        const double distSquared = (doublePosition - center_).LengthSquared();
        return Sqrt(distSquared) - radius_;
    }
};

/// Surface triangle of tetrahedral mesh with adjacency information.
struct TetrahedralMeshSurfaceTriangle
{
    /// Indices of triangle vertices.
    unsigned indices_[3]{};
    /// Index of the 4th vertex of underlying tetrahedron. Unspecified if there's no underlying tetrahedron.
    unsigned unusedIndex_{ M_MAX_UNSIGNED };
    /// Indices of neighbor triangles.
    unsigned neighbors_[3]{ M_MAX_UNSIGNED, M_MAX_UNSIGNED, M_MAX_UNSIGNED};
    /// Index of underlying tetrahedron. M_MAX_UNSIGNED if empty.
    unsigned tetIndex_{ M_MAX_UNSIGNED };
    /// Face of underlying tetrahedron, from 0 to 3.
    unsigned tetFace_{};

    /// Return edge, from 0 to 2. Returned indices are sorted.
    ea::pair<unsigned, unsigned> GetEdge(unsigned edgeIndex) const
    {
        unsigned begin = indices_[edgeIndex];
        unsigned end = indices_[(edgeIndex + 1) % 3];
        if (begin > end)
            ea::swap(begin, end);
        return { begin, end };
    }

    /// Return whether the triangle has given neighbour.
    bool HasNeighbor(unsigned neighborIndex) const
    {
        return ea::count(ea::begin(neighbors_), ea::end(neighbors_), neighborIndex) != 0;
    }

    /// Normalize triangle indices so (p2 - p1) x (p3 - p1) is the normal.
    void Normalize(const ea::vector<Vector3>& vertices)
    {
        const Vector3 p0 = vertices[unusedIndex_];
        const Vector3 p1 = vertices[indices_[0]];
        const Vector3 p2 = vertices[indices_[1]];
        const Vector3 p3 = vertices[indices_[2]];
        const Vector3 outsideDirection = p1 - p0;
        const Vector3 actualNormal = (p2 - p1).CrossProduct(p3 - p1);
        if (outsideDirection.DotProduct(actualNormal) < 0.0f)
        {
            ea::swap(indices_[0], indices_[1]);
            ea::swap(neighbors_[0], neighbors_[1]);
        }
    }

    /// Calculate the ratio between longest and shortest side of the triangle.
    float CalculateScore(const ea::vector<Vector3>& vertices) const
    {
        const Vector3 p1 = vertices[indices_[0]];
        const Vector3 p2 = vertices[indices_[1]];
        const Vector3 p3 = vertices[indices_[2]];

        const float side1 = (p1 - p2).Length();
        const float side2 = (p2 - p3).Length();
        const float side3 = (p3 - p1).Length();

        const auto minMaxSide = ea::minmax({ side1, side2, side3 });
        return ea::min(M_LARGE_VALUE, minMaxSide.second / minMaxSide.first);
    }
};

/// Edge of the surface of tetrahedral mesh.
struct TetrahedralMeshSurfaceEdge
{
    /// Indices.
    unsigned indices_[2]{};
    /// Face that owns this edge.
    unsigned faceIndex_{ M_MAX_UNSIGNED };
    /// Index of the edge in triangle.
    unsigned edgeIndex_{};

    /// Construct default.
    TetrahedralMeshSurfaceEdge() = default;

    /// Construct valid.
    TetrahedralMeshSurfaceEdge(unsigned i0, unsigned i1, unsigned faceIndex, float edgeIndex)
        : indices_{ i0, i1 }
        , faceIndex_(faceIndex)
        , edgeIndex_(edgeIndex)
    {
        if (indices_[0] > indices_[1])
            ea::swap(indices_[0], indices_[1]);
    }

    /// Compare for sorting. Only edges themselves are compared.
    bool operator < (const TetrahedralMeshSurfaceEdge& rhs) const
    {
        if (indices_[0] != rhs.indices_[0])
            return indices_[0] < rhs.indices_[0];
        return indices_[1] < rhs.indices_[1];
    }
};

/// Surface of tetrahedral mesh. Vertices are shared with tetrahedral mesh and are not stored.
struct TetrahedralMeshSurface
{
    /// Faces.
    ea::vector<TetrahedralMeshSurfaceTriangle> faces_;

    /// Temporary buffer for calculating adjacency.
    ea::vector<TetrahedralMeshSurfaceEdge> edges_;

    /// Clear.
    void Clear() { faces_.clear(); }

    /// Return size.
    unsigned Size() const { return faces_.size(); }

    /// Calculate adjacency information. Surface must be closed.
    bool CalculateAdjacency();

    /// Return whether the mesh is a closed surface.
    bool IsClosedSurface() const;
};

/// Tetrahedron with adjacency information.
struct Tetrahedron
{
    /// Special index for infinite vertex of outer tetrahedron, cubic equation.
    static const unsigned Infinity3 = M_MAX_UNSIGNED;
    /// Special index for infinite vertex of outer tetrahedron, square equation.
    static const unsigned Infinity2 = M_MAX_UNSIGNED - 1;

    /// Indices of tetrahedron vertices.
    unsigned indices_[4]{};
    /// Indices of neighbor tetrahedrons. M_MAX_UNSIGNED if empty.
    unsigned neighbors_[4]{ M_MAX_UNSIGNED, M_MAX_UNSIGNED, M_MAX_UNSIGNED, M_MAX_UNSIGNED };
    /// Pre-computed matrix for calculating barycentric coordinates.
    Matrix3x4 matrix_;

    /// Calculate matrix for valid inner tetrahedron.
    void CalculateInnerMatrix(const ea::vector<Vector3>& vertices)
    {
        const Vector3 p0 = vertices[indices_[0]];
        const Vector3 p1 = vertices[indices_[1]];
        const Vector3 p2 = vertices[indices_[2]];
        const Vector3 p3 = vertices[indices_[3]];
        const Vector3 u1 = p1 - p0;
        const Vector3 u2 = p2 - p0;
        const Vector3 u3 = p3 - p0;
        matrix_ = Matrix3(u1.x_, u2.x_, u3.x_, u1.y_, u2.y_, u3.y_, u1.z_, u2.z_, u3.z_).Inverse();
    }

    /// Return indices of specified triangle face of the tetrahedron.
    void GetTriangleFaceIndices(unsigned faceIndex, unsigned (&indices)[3]) const
    {
        unsigned j = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            if (i != faceIndex)
                indices[j++] = indices_[i];
        }
    }

    /// Return triangle face of the tetrahedron. Adjacency information is left uninitialized.
    TetrahedralMeshSurfaceTriangle GetTriangleFace(unsigned faceIndex, unsigned tetIndex, unsigned tetFace) const
    {
        TetrahedralMeshSurfaceTriangle face;
        GetTriangleFaceIndices(faceIndex, face.indices_);
        face.unusedIndex_ = indices_[faceIndex];
        face.tetIndex_ = tetIndex;
        face.tetFace_ = tetFace;
        return face;
    }

    /// Return face index corresponding to given neighbor. Return 4 if not found.
    unsigned GetNeighborFaceIndex(unsigned neighborTetIndex) const
    {
        return ea::find(ea::begin(neighbors_), ea::end(neighbors_), neighborTetIndex) - ea::begin(neighbors_);
    }

    /// Return whether the tetrahedron has given neighbour.
    bool HasNeighbor(unsigned neighborTetIndex) const
    {
        return GetNeighborFaceIndex(neighborTetIndex) < 4;
    }
};

/// Tetrahedral mesh.
class URHO3D_API TetrahedralMesh
{
public:
    /// Define mesh from vertices.
    void Define(ea::span<const Vector3> positions);

    /// Collect all edges in the mesh, e.g. for debug rendering.
    void CollectEdges(ea::vector<ea::pair<unsigned, unsigned>>& edges);

    /// Calculate circumsphere of given tetrahedron.
    HighPrecisionSphere GetTetrahedronCircumsphere(unsigned tetIndex) const;

    /// Calculate barycentric coordinates for inner tetrahedron.
    Vector4 GetInnerBarycentricCoords(unsigned tetIndex, const Vector3& position) const
    {
        const Tetrahedron& tetrahedron = tetrahedrons_[tetIndex];
        const Vector3& basePosition = vertices_[tetrahedron.indices_[0]];
        const Vector3 coords = tetrahedron.matrix_ * (position - basePosition);
        return { 1.0f - coords.x_ - coords.y_ - coords.z_, coords.x_, coords.y_, coords.z_ };
    }

    /// Calculate barycentric coordinates for outer tetrahedron.
    Vector4 GetOuterBarycentricCoords(unsigned tetIndex, const Vector3& position) const
    {
        const Tetrahedron& tetrahedron = tetrahedrons_[tetIndex];
        const Vector3 p1 = vertices_[tetrahedron.indices_[0]];
        const Vector3 p2 = vertices_[tetrahedron.indices_[1]];
        const Vector3 p3 = vertices_[tetrahedron.indices_[2]];
        const Vector3 normal = (p2 - p1).CrossProduct(p3 - p1);

        // Position is in the inner cell, return fake barycentric
        if (normal.DotProduct(position - p1) < 0.0f)
            return { 0.0f, 0.0f, 0.0f, -1.0f };

        const Vector3 poly = tetrahedron.matrix_ * position;
        const float t = tetrahedron.indices_[3] == Tetrahedron::Infinity3 ? SolveCubic(poly) : SolveQuadratic(poly);

        const Vector3 t1 = p1 + t * hullNormals_[tetrahedron.indices_[0]];
        const Vector3 t2 = p2 + t * hullNormals_[tetrahedron.indices_[1]];
        const Vector3 t3 = p3 + t * hullNormals_[tetrahedron.indices_[2]];
        const Vector3 coords = GetTriangleBarycentricCoords(position, t1, t2, t3);
        return { coords, 0.0f };
    }

    /// Calculate barycentric coordinates for tetrahedron.
    Vector4 GetBarycentricCoords(unsigned tetIndex, const Vector3& position) const
    {
        return tetIndex < numInnerTetrahedrons_
            ? GetInnerBarycentricCoords(tetIndex, position)
            : GetOuterBarycentricCoords(tetIndex, position);
    }

    /// Find tetrahedron containing given position and calculate barycentric coordinates within this tetrahedron.
    Vector4 GetInterpolationFactors(const Vector3& position, unsigned& tetIndexHint) const
    {
        if (tetrahedrons_.empty())
            return Vector4::ZERO;

        const unsigned maxIters = tetrahedrons_.size();
        if (tetIndexHint >= maxIters)
            tetIndexHint = 0;

        for (unsigned i = 0; i < maxIters; ++i)
        {
            const Vector4 weights = GetBarycentricCoords(tetIndexHint, position);
            if (weights.x_ >= 0.0f && weights.y_ >= 0.0f && weights.z_ >= 0.0f && weights.w_ >= 0.0f)
                return weights;

            if (weights.x_ < weights.y_ && weights.x_ < weights.z_ && weights.x_ < weights.w_)
                tetIndexHint = tetrahedrons_[tetIndexHint].neighbors_[0];
            else if (weights.y_ < weights.z_ && weights.y_ < weights.w_)
                tetIndexHint = tetrahedrons_[tetIndexHint].neighbors_[1];
            else if (weights.z_ < weights.w_)
                tetIndexHint = tetrahedrons_[tetIndexHint].neighbors_[2];
            else
                tetIndexHint = tetrahedrons_[tetIndexHint].neighbors_[3];
        }
        return GetBarycentricCoords(tetIndexHint, position);
    }

    /// Sample value at given position from the arbitrary container of per-vertex data.
    template <class Container>
    auto Sample(const Container& container, const Vector3& position, unsigned& tetIndexHint) const
    {
        typename Container::value_type result{};

        const Vector4& weights = GetInterpolationFactors(position, tetIndexHint);
        if (tetIndexHint < tetrahedrons_.size())
        {
            const Tetrahedron& tetrahedron = tetrahedrons_[tetIndexHint];
            for (unsigned i = 0; i < 3; ++i)
                result += container[tetrahedron.indices_[i]] * weights[i];
            if (tetIndexHint < numInnerTetrahedrons_)
                result += container[tetrahedron.indices_[3]] * weights[3];
        }
        return result;
    }

private:
    /// Solve cubic equation x^3 + a*x^2 + b*x + c = 0.
    static int SolveCubicEquation(double result[], double a, double b, double c, double eps)
    {
        double a2 = a * a;
        double q = (a2 - 3 * b) / 9;
        double r = (a * (2 * a2 - 9 * b) + 27 * c) / 54;
        double r2 = r * r;
        double q3 = q * q * q;
        if (r2 <= (q3 + eps))
        {
            double t = r / std::sqrt(q3);
            if (t < -1)
                t = -1;
            if (t > 1)
                t = 1;
            t = std::acos(t);
            a /= 3;
            q = -2 * std::sqrt(q);
            result[0] = q * std::cos(t / 3) - a;
            result[1] = q * std::cos((t + M_PI * 2) / 3) - a;
            result[2] = q * std::cos((t - M_PI * 2) / 3) - a;
            return 3;
        }
        else
        {
            double A = -std::cbrt(std::abs(r) + std::sqrt(r2 - q3));
            if (r < 0)
                A = -A;
            const double B = A == 0 ? 0 : q / A;

            a /= 3;
            result[0] = (A + B) - a;
            result[1] = -0.5 * (A + B) - a;
            result[2] = 0.5 * std::sqrt(3.0) * (A - B);
            if (std::abs(result[2]) < eps)
            {
                result[2] = result[1];
                return 2;
            }
            return 1;
        }
    }

    /// Calculate most positive root of cubic equation x^3 + a*x^2 + b*x + c = 0.
    static float SolveCubic(const Vector3& abc)
    {
        double result[3];
        const int numRoots = SolveCubicEquation(result, abc.x_, abc.y_, abc.z_, M_EPSILON);
        return static_cast<float>(*ea::max_element(result, result + numRoots));
    }

    /// Calculate most positive root of quadratic or linear equation a*x^2 + b*x + c = 0.
    static float SolveQuadratic(const Vector3& abc)
    {
        const float a = abc.x_;
        const float b = abc.y_;
        const float c = abc.z_;
        if (std::abs(a) < M_EPSILON)
            return -c / b;

        const float D = ea::max(0.0f, b * b - 4 * a * c);

        return a > 0
            ? (-b + std::sqrt(D)) / (2 * a)
            : (-b - std::sqrt(D)) / (2 * a);
    }

    /// Calculate barycentric coordinates on triangle.
    static Vector3 GetTriangleBarycentricCoords(const Vector3& position,
        const Vector3& p1, const Vector3& p2, const Vector3& p3)
    {
        const Vector3 v12 = p2 - p1;
        const Vector3 v13 = p3 - p1;
        const Vector3 v0 = position - p1;
        const float d00 = v12.DotProduct(v12);
        const float d01 = v12.DotProduct(v13);
        const float d11 = v13.DotProduct(v13);
        const float d20 = v0.DotProduct(v12);
        const float d21 = v0.DotProduct(v13);
        const float denom = d00 * d11 - d01 * d01;
        const float v = (d11 * d20 - d01 * d21) / denom;
        const float w = (d00 * d21 - d01 * d20) / denom;
        const float u = 1.0f - v - w;
        return { u, v, w };
    }

    /// Find tetrahedron for given position. Ignore removed tetrahedrons. Return invalid index if cannot find.
    unsigned FindTetrahedron(const Vector3& position, ea::vector<bool>& removed) const
    {
        auto firstNotRemovedIter = ea::find(removed.begin(), removed.end(), false);
        if (firstNotRemovedIter == removed.end())
            return M_MAX_UNSIGNED;

        const unsigned maxIters = tetrahedrons_.size();
        unsigned tetIndex = firstNotRemovedIter - removed.begin();
        for (unsigned i = 0; i < maxIters; ++i)
        {
            // Found one
            const Vector4 weights = GetInnerBarycentricCoords(tetIndex, position);
            if (weights.x_ >= 0.0f && weights.y_ >= 0.0f && weights.z_ >= 0.0f && weights.w_ >= 0.0f)
                break;

            if (weights.x_ < weights.y_ && weights.x_ < weights.z_ && weights.x_ < weights.w_)
                tetIndex = tetrahedrons_[tetIndex].neighbors_[0];
            else if (weights.y_ < weights.z_ && weights.y_ < weights.w_)
                tetIndex = tetrahedrons_[tetIndex].neighbors_[1];
            else if (weights.z_ < weights.w_)
                tetIndex = tetrahedrons_[tetIndex].neighbors_[2];
            else
                tetIndex = tetrahedrons_[tetIndex].neighbors_[3];

            // Failed to find one
            if (tetIndex == M_MAX_UNSIGNED)
                break;
        }
        return tetIndex;
    }

    /// Number of initial super-mesh vertices.
    static const unsigned NumSuperMeshVertices = 8;
    /// Create super-mesh for Delaunay triangulation.
    void InitializeSuperMesh(const BoundingBox& boundingBox);
    /// Build tetrahedrons for given positions.
    void BuildTetrahedrons(ea::span<const Vector3> positions);
    /// Return whether the adjacency is valid.
    bool IsAdjacencyValid(bool fullyConnected) const;
    /// Disconnect tetrahedron from mesh.
    void DisconnectTetrahedron(unsigned tetIndex);

    /// Data used for Delaunay triangulation.
    struct DelaunayContext
    {
        /// Circumspheres of mesh tetrahedrons.
        ea::vector<HighPrecisionSphere> circumspheres_;
        /// Whether the tetrahedron is removed.
        ea::vector<bool> removed_;
        /// Tests if point is inside circumsphere of tetrahedron.
        bool IsInsideCircumsphere(unsigned tetIndex, const Vector3& position)
        {
            const HighPrecisionSphere& sphere = circumspheres_[tetIndex];
            return sphere.Distance(position) < M_LARGE_EPSILON;
        }
    };

    /// Find and remove (aka set removed flag) tetrahedrons whose circumspheres intersect given point.
    /// Returns hole surface. Returns true on success. Mesh remains valid in case of failure.
    bool FindAndRemoveIntersected(DelaunayContext& ctx, const Vector3& position,
        TetrahedralMeshSurface& holeSurface, ea::vector<unsigned>& removedTetrahedrons, bool dumpErrors = false) const;
    /// Disconnect removed tetrahedrons from the rest.
    void DisconnectRemovedTetrahedrons(const ea::vector<unsigned>& removedTetrahedrons);
    /// Fill star-shaped hole with tetrahedrons connected to specified vertex.
    /// Output tetrahedrons should be allocated beforehand.
    void FillStarShapedHole(DelaunayContext& ctx, const ea::vector<unsigned>& outputTetrahedrons,
        const TetrahedralMeshSurface& holeSurface, unsigned centerIndex);
    /// Mark super-mesh tetrahedrons in the to-be-removed array and disconnect all related adjacency.
    void DisconnectSuperMeshTetrahedrons(ea::vector<bool>& removed);
    /// Ensure mesh connectivity, remove disconnected parts.
    void EnsureMeshConnectivity(ea::vector<bool>& removed);
    /// Collect surface tetrahedrons and ensure that the surface doesn't have edge connections.
    void FilterMeshSurface(ea::vector<bool>& removed);
    /// Remove marked tetrahedrons from array.
    void RemoveMarkedTetrahedrons(const ea::vector<bool>& removed);
    /// Remove super-mesh vertices.
    void RemoveSuperMeshVertices();
    /// Update array of ignored vertices.
    void UpdateIgnoredVertices();

    /// Build hull surface.
    void BuildHullSurface(TetrahedralMeshSurface& hullSurface);
    /// Calculate hull normals.
    void CalculateHullNormals(const TetrahedralMeshSurface& hullSurface);
    /// Build outer tetrahedrons.
    void BuildOuterTetrahedrons(const TetrahedralMeshSurface& hullSurface);
    /// Calculate matrices for outer tetrahedrons.
    void CalculateOuterMatrices();

public:
    /// Vertices.
    ea::vector<Vector3> vertices_;
    /// Tetrahedrons.
    ea::vector<Tetrahedron> tetrahedrons_;
    /// Hull normals.
    ea::vector<Vector3> hullNormals_;
    /// Array of ignored vertices.
    ea::vector<unsigned> ignoredVertices_;
    /// Number of inner tetrahedrons.
    unsigned numInnerTetrahedrons_{};

    /// Debug array of edges related to errors in generation.
    mutable ea::vector<ea::pair<unsigned, unsigned>> debugHighlightEdges_;
};

/// Serialize tetrahedron to archive.
URHO3D_API bool SerializeValue(Archive& archive, const char* name, Tetrahedron& value);

/// Serialize tetrahedral mesh to archive.
URHO3D_API bool SerializeValue(Archive& archive, const char* name, TetrahedralMesh& value);

}