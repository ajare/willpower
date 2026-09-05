#include <poly2tri/poly2tri.h>

#include <cmath>

#include "willpower/common/Globals.h"
#include "willpower/common/WillpowerWalker.h"
#include "willpower/common/polypartition.h"

#include "willpower/geometry/MeshUtils.h"

#include "willpower/wayfinder/Floor.h"
#include "willpower/wayfinder/ConvexPolygonisation.h"

#undef max

namespace WP_NAMESPACE
{
	namespace wayfinder
	{

		using namespace std;
		using namespace WP_NAMESPACE;

		namespace
		{
			float polygonArea(vector<Vector2> const& vertices)
			{
				float twiceArea = 0.0f;
				for (size_t i = 0; i < vertices.size(); ++i)
				{
					auto const& current = vertices[i];
					auto const& next = vertices[(i + 1) % vertices.size()];
					twiceArea += current.x * next.y - current.y * next.x;
				}
				return std::abs(twiceArea) * 0.5f;
			}
		}

		ConvexPolygonisation::ConvexPolygonisation()
			: mPolygonAccelerationGrid(nullptr)
		{
		}

		ConvexPolygonisation::~ConvexPolygonisation()
		{
			delete mPolygonAccelerationGrid;
		}

		void ConvexPolygonisation::addArea(vector<Vector2> const& border, vector<vector<Vector2>> const& holes)
		{
			polygonise1(border, holes);
		}

		vector<Vector2> ConvexPolygonisation::processLoop(vector<Vector2> const& points)
		{
			float epsilonSq = MathsUtils::Epsilon;

			vector<Vector2> processedPoints;
			for (uint32_t i = 0; i < points.size(); ++i)
			{
				Vector2 const& vertex = points[i];

				// Don't allow multiple vertices too close to each other as this can
				// crash the triangulator.
				if (i > 0 && vertex.distanceToSq(points[i - 1]) < epsilonSq)
				{
					continue;
				}

				// Check last point against first
				if (i == points.size() - 1)
				{
					if (vertex.distanceToSq(points[0]) < epsilonSq)
					{
						continue;
					}
				}

				// Don't allow collinear points, ie three or more on a straight line.
				if (i > 0 && MathsUtils::pointsFormLine(points, i - 1))
				{
					continue;
				}

				processedPoints.push_back(vertex);
			}

			return processedPoints;
		}

		VertexIndex ConvexPolygonisation::addVertex(float x, float y)
		{
			VertexIndex index = (VertexIndex)mVertices.size();

			mVertices.push_back(Vector2(x, y));

			return index;
		}

		uint32_t ConvexPolygonisation::addTriangle(VertexIndex vertices[3])
		{
			PolygonIndex polygonIndex = (PolygonIndex)mPolygons.size();

			Polygon p;
			p.degree = 3;

			for (uint32_t i = 0; i < 3; ++i)
			{
				VertexIndex v0 = vertices[i];
				VertexIndex v1 = vertices[i == 2 ? 0 : i + 1];

				// See if first edge already exists
				auto it = mVertexToEdgeLookup.find((v0 << 16) + v1);
				if (it != mVertexToEdgeLookup.end())
				{
					auto& edge = mEdges[it->second];

					// Calculate side from edge.v[0] to edge.v[1]
					edge.p[1] = (PossiblePolygonIndex)polygonIndex;

					// p[0] and p[1] should be ordered based on
					// their side of the line that forms edge.v[0] to edge.v[1]
					// Side::Right should be p[0] and Side::Left should be p[1]
					int otherIndex = 0;
					Vector2 poly1centre = (mVertices[vertices[0]] + mVertices[vertices[1]] + mVertices[vertices[2]]) / 3;
					if (MathsUtils::pointSideOnLine(poly1centre, mVertices[edge.v[0]], mVertices[edge.v[1]]) == MathsUtils::Side::Right)
					{
						swap(edge.p[0], edge.p[1]);
						otherIndex = 1;
					}

					if (edge.v[0] == (PossibleVertexIndex)v0 && edge.v[1] == (PossibleVertexIndex)v1)
					{
						p.vertices[i] = (VertexIndex)edge.v[0];
					}
					else
					{
						p.vertices[i] = (VertexIndex)edge.v[1];
					}

					// Set neighbours
					p.neighbours[p.numNeighbours++] = (PolygonIndex)edge.p[otherIndex];

					auto& otherPoly = mPolygons[edge.p[otherIndex]];
					otherPoly.neighbours[otherPoly.numNeighbours++] = polygonIndex;
				}
				else
				{
					// Create new edge
					EdgeIndex edgeIndex = (EdgeIndex)mEdges.size();

					Edge e;
					e.v[0] = (PossibleVertexIndex)v0;
					e.v[1] = (PossibleVertexIndex)v1;
					e.p[0] = (PossiblePolygonIndex)polygonIndex;

					mEdges.push_back(e);

					// This code will break if VertexIndex type is changed.
					mVertexToEdgeLookup[(v0 << 16) + v1] = edgeIndex;
					mVertexToEdgeLookup[(v1 << 16) + v0] = edgeIndex;

					p.vertices[i] = v0;
				}
			}

			mPolygons.push_back(p);

			return polygonIndex;
		}

		void ConvexPolygonisation::buildPathGraph()
		{
			mPathGraph.clear();
			mPathGraph.reserve(mPolygons.size());

			for (uint32_t polyIndex = 0; polyIndex < mPolygons.size(); ++polyIndex)
			{
				auto const& sourcePoly = mPolygons[polyIndex];

				// Get source polygon centre
				Vector2 sourceCentre = sourcePoly.getCentre(this);

				mPathGraph.push_back(vector<GraphEdge>());

				for (uint32_t i = 0; i < sourcePoly.numNeighbours; ++i)
				{
					auto const& targetPoly = mPolygons[sourcePoly.neighbours[i]];

					Vector2 targetCentre = targetPoly.getCentre(this);

					GraphEdge graphEdge;

					graphEdge.distance = (uint32_t)sourceCentre.distanceTo(targetCentre);
					graphEdge.target = sourcePoly.neighbours[i];

					mPathGraph[polyIndex].push_back(graphEdge);
				}
			}
		}

		void ConvexPolygonisation::createAccelerationGrids(int dimX, int dimY)
		{
			if (mPolygonAccelerationGrid)
			{
				delete mPolygonAccelerationGrid;
			}

			BoundingBox polyBounds = MathsUtils::getPolygonBounds(mVertices);

			mPolygonAccelerationGrid = new AccelerationGrid(polyBounds.getPosition(), polyBounds.getSize(), dimX, dimY);

			for (uint32_t i = 0; i < mPolygons.size(); ++i)
			{
				// Calculate polygon bounds
				vector<Vector2> vertices;

				for (uint32_t j = 0; j < mPolygons[i].degree; ++j)
				{
					vertices.push_back(mVertices[mPolygons[i].vertices[j]]);
				}

				wp::BoundingBox bb = MathsUtils::getPolygonBounds(vertices);

				mPolygonAccelerationGrid->addItem(i, bb);
			}
		}


		void ConvexPolygonisation::calculatePaths(PolygonIndex target, PathDatabase* database) const
		{
			vector<uint32_t> minDistance(mPathGraph.size(), numeric_limits<uint32_t>::max());
			minDistance[target] = 0;

			set<pair<uint32_t, PolygonIndex>> activePolygons;
			activePolygons.insert(make_pair(0, target));

			while (!activePolygons.empty())
			{
				PolygonIndex where = activePolygons.begin()->second;

				activePolygons.erase(activePolygons.begin());

				for (auto const& edge: mPathGraph[where])
				{
					if (minDistance[edge.target] > minDistance[where] + edge.distance)
					{
						activePolygons.erase(make_pair(minDistance[edge.target], edge.target));
						minDistance[edge.target] = minDistance[where] + edge.distance;

						// Store portal to next polygon in path
						EdgeIndex joiningEdgeIndex = mPolygonToEdgeLookup.at((edge.target << 16) + where);

						PolygonIndex sourcePolygon = edge.target;
						PolygonIndex targetPolygon = target;
						PolygonIndex prevPolygon = where;

						// Only add in one direction
						VertexIndex* basePtr = database->getHead();
						VertexIndex* ptr = basePtr;

						if (sourcePolygon < targetPolygon)
						{
							uint32_t newLength = 1;
							*ptr++ = joiningEdgeIndex;

							if (prevPolygon != targetPolygon)
							{
								if (prevPolygon < targetPolygon)
								{
									auto const& prevEntry = database->getEntry(prevPolygon, targetPolygon);
									for (uint32_t i = 0; i < prevEntry.length; ++i)
									{
										*ptr++ = prevEntry.base[i];
									}

									newLength += prevEntry.length;
								}
								else if (prevPolygon > targetPolygon)
								{
									auto const& prevEntry = database->getEntry(targetPolygon, prevPolygon);
									for (uint32_t i = 0; i < prevEntry.length; ++i)
									{
										*ptr++ = prevEntry.base[prevEntry.length - i - 1];
									}

									newLength += prevEntry.length;
								}
							}

							database->createEntry(sourcePolygon, targetPolygon, basePtr, newLength);
						}
						else
						{
							// Add in reverse
							uint32_t newLength = 0;
							if (prevPolygon != targetPolygon)
							{
								if (prevPolygon < targetPolygon)
								{
									auto const& prevEntry = database->getEntry(prevPolygon, targetPolygon);
									for (uint32_t i = 0; i < prevEntry.length; ++i)
									{
										*ptr++ = prevEntry.base[prevEntry.length - i - 1];
									}

									newLength += prevEntry.length;
								}
								else if (prevPolygon > targetPolygon)
								{
									auto const& prevEntry = database->getEntry(targetPolygon, prevPolygon);
									for (uint32_t i = 0; i < prevEntry.length; ++i)
									{
										*ptr++ = prevEntry.base[i];
									}

									newLength += prevEntry.length;
								}
							}

							*ptr++ = joiningEdgeIndex;
							newLength++;

							database->createEntry(targetPolygon, sourcePolygon, basePtr, newLength);
						}

						activePolygons.insert(make_pair(minDistance[edge.target], edge.target));
					}
				}
			}

			database->setCalculated(target);
		}

		vector<int> ConvexPolygonisation::calculatePathLengths(PolygonIndex target) const
		{
			vector<uint32_t> minDistance(mPathGraph.size(), numeric_limits<uint32_t>::max());
			minDistance[target] = 0;

			// Result paths
			vector<int> lengths(mPathGraph.size(), 0);

			// Target poly needs to be zero
			lengths[target] = 0;

			set<pair<PolygonIndex, PolygonIndex>> activePolygons;
			activePolygons.insert(make_pair((PolygonIndex)0, target));

			while (!activePolygons.empty())
			{
				PolygonIndex where = activePolygons.begin()->second;

				activePolygons.erase(activePolygons.begin());

				for (auto const& edge: mPathGraph[where])
				{
					if (minDistance[edge.target] > minDistance[where] + edge.distance)
					{
						activePolygons.erase(make_pair((PolygonIndex)minDistance[edge.target], (PolygonIndex)edge.target));
						minDistance[edge.target] = minDistance[where] + edge.distance;

						lengths[edge.target] = 1 + lengths[where];
						activePolygons.insert(make_pair((PolygonIndex)minDistance[edge.target], (PolygonIndex)edge.target));
					}
				}
			}

			return lengths;
		}

		void ConvexPolygonisation::polygonise1(vector<Vector2> const& border, vector<vector<Vector2>> const& holes)
		{
			vector<Vector2> processedBorder = processLoop(border);

			vector<p2t::Point*> borderPoints;
			for (uint32_t i = 0; i < processedBorder.size(); ++i)
			{
				borderPoints.push_back(new p2t::Point(processedBorder[i].x, processedBorder[i].y));
			}

			// Triangulate
			p2t::CDT* cdt = new p2t::CDT(borderPoints);

			vector <vector<p2t::Point*>> holeList;
			for (auto const& hole: holes)
			{
				auto processedHole = processLoop(hole);

				vector<p2t::Point*> holePoints;
				for (uint32_t i = 0; i < hole.size(); ++i)
				{
					holePoints.push_back(new p2t::Point(processedHole[i].x, processedHole[i].y));
				}

				holeList.push_back(holePoints);
				cdt->AddHole(holePoints);
			}

			cdt->Triangulate();

			// Extract triangle data
			map<p2t::Point*, VertexIndex> vertexMap;

			auto const& triangles = cdt->GetTriangles();
			for (auto const& triangle: triangles)
			{
				VertexIndex triangleIndices[3];
				for (int i = 0; i < 3; ++i)
				{
					p2t::Point* p = triangle->GetPoint(i);
					if (vertexMap.find(p) == vertexMap.end())
					{
						// Add new vertex
						vertexMap[p] = addVertex((float)p->x, (float)p->y);
					}

					triangleIndices[i] = vertexMap[p];
				}

				// Ensure anticlockwise winding.
				if (MathsUtils::pointSideOnLine(
					mVertices[triangleIndices[0]],
					mVertices[triangleIndices[1]],
					mVertices[triangleIndices[2]]) == MathsUtils::Side::Right)
				{
					swap(triangleIndices[0], triangleIndices[1]);
				}

				// Add new triangle
				addTriangle(&triangleIndices[0]);
			}

			delete cdt;

			// Clear points
			for (auto point: borderPoints)
			{
				delete point;
			}

			for (auto holePoints: holeList)
			{
				for (auto point: holePoints)
				{
					delete point;
				}
			}
		}

		void ConvexPolygonisation::polygonise2(vector<Vector2> const& border, vector<vector<Vector2>> const& holes)
		{
			mVertices.clear();
			mEdges.clear();
			mVertexToEdgeLookup.clear();
			mPolygons.clear();

			// Polygonise using polypartition
			list<TPPLPoly> polys;

			vector<Vector2> processedBorder = processLoop(border);

			TPPLPoly poly;
			poly.Init((long)processedBorder.size());
			poly.SetHole(false);
			poly.SetOrientation(TPPL_CCW);

			for (uint32_t i = 0; i < processedBorder.size(); ++i)
			{
				poly[i].x = processedBorder[i].x;
				poly[i].y = processedBorder[i].y;
			}

			polys.push_back(poly);

			for (auto const& hole : holes)
			{
				auto processedHole = processLoop(hole);

				TPPLPoly holePoly;
				holePoly.Init((long)processedHole.size());
				holePoly.SetHole(true);
				holePoly.SetOrientation(TPPL_CW);

				for (uint32_t j = 0; j < processedHole.size(); ++j)
				{
					holePoly[j].x = processedHole[j].x;
					holePoly[j].y = processedHole[j].y;
				}

				polys.push_back(holePoly);
			}


			TPPLPartition partitioner;

			list<TPPLPoly> result;

			partitioner.Triangulate_EC(&polys, &result);
			//partitioner.Triangulate_MONO(&polys, &result);

			// Extract triangle data: result is a list of triangles specified by vertex positions, so need
			// to index them somehow.  No new points are created though, so output points are guaranteed to
			// be the same as input, so direct comparison should be enough.
			map<Vector2, VertexIndex, Vector2Compare> vertexMap;

			for (auto& res: result)
			{
				vector<VertexIndex> triangleIndices;

				for (long i = 0; i < res.GetNumPoints(); ++i)
				{
					TPPLPoint const& p = res.GetPoint(i);
					Vector2 vertex((float)p.x, (float)p.y);

					if (vertexMap.find(vertex) == vertexMap.end())
					{
						vertexMap[vertex] = addVertex(vertex.x, vertex.y);
					}

					triangleIndices.push_back(vertexMap[vertex]);
				}

				// Ensure anticlockwise winding.
				if (MathsUtils::pointSideOnLine(
					mVertices[triangleIndices[0]],
					mVertices[triangleIndices[1]],
					mVertices[triangleIndices[2]]) == MathsUtils::Side::Right)
				{
					swap(triangleIndices[0], triangleIndices[1]);
				}

				// Add new triangle
				addTriangle(&triangleIndices[0]);
			}
		}

		void ConvexPolygonisation::composeConvex(unsigned int maxDegree)
		{
			bool edgesToRemove = true;

			if (maxDegree > MaxPolygonDegree)
			{
				maxDegree = MaxPolygonDegree;
			}

			VertexIndex maxVertexIndices[MaxPolygonDegree];
			uint32_t maxVertexCount{ 0 };

			while (edgesToRemove)
			{
				float maxArea = -1e10f;
				PossibleEdgeIndex maxIndex = NoIndex;

				edgesToRemove = false;

				for (EdgeIndex edgeIndex = 0; edgeIndex < mEdges.size(); ++edgeIndex)
				{
					auto const& edge = mEdges[edgeIndex];

					// Ignore borders
					if (edge.p[1] < 0)
					{
						continue;
					}

					auto const& p0 = mPolygons[edge.p[0]];
					auto const& p1 = mPolygons[edge.p[1]];

					// Don't merge if result would have too many polygon
					if ((p0.degree + p1.degree) > maxDegree)
					{
						continue;
					}

					// Copy vertex lists and rotate so that edge vertices are the first two
					VertexIndex rotatedVertices0[MaxPolygonDegree], rotatedVertices1[MaxPolygonDegree];

					memcpy(rotatedVertices0, p0.vertices, p0.degree * sizeof(VertexIndex));
					memcpy(rotatedVertices1, p1.vertices, p1.degree * sizeof(VertexIndex));

					for (uint32_t i = 0; i < p0.degree; ++i)
					{
						if (p0.vertices[i] == (VertexIndex)edge.v[0] || p0.vertices[i] == (VertexIndex)edge.v[1])
						{
							if (i == 0 && (p0.vertices[p0.degree - 1] == (VertexIndex)edge.v[0] || p0.vertices[p0.degree - 1] == (VertexIndex)edge.v[1]))
							{
								rotate(rotatedVertices0, rotatedVertices0 + (p0.degree - 1), rotatedVertices0 + p0.degree);
							}
							else
							{
								rotate(rotatedVertices0, rotatedVertices0 + i, rotatedVertices0 + p0.degree);
							}
							break;
						}
					}

					for (uint32_t i = 0; i < p1.degree; ++i)
					{
						if (p1.vertices[i] == (VertexIndex)edge.v[0] || p1.vertices[i] == (VertexIndex)edge.v[1])
						{
							if (i == 0 && (p1.vertices[p1.degree - 1] == (VertexIndex)edge.v[0] || p1.vertices[p1.degree - 1] == (VertexIndex)edge.v[1]))
							{
								rotate(rotatedVertices1, rotatedVertices1 + (p1.degree - 1), rotatedVertices1 + p1.degree);
							}
							else
							{
								rotate(rotatedVertices1, rotatedVertices1 + i, rotatedVertices1 + p1.degree);
							}
							break;
						}
					}

					// Insert rotatedVertices1[2] onwards after rotatedVertices[0]
					vector<VertexIndex> mergedIndices;
					vector<Vector2> mergedVertices;

					mergedIndices.push_back(rotatedVertices0[0]);
					mergedVertices.push_back(mVertices[rotatedVertices0[0]]);

					for (uint32_t i = 2; i < p1.degree; ++i)
					{
						mergedIndices.push_back(rotatedVertices1[i]);
						mergedVertices.push_back(mVertices[rotatedVertices1[i]]);
					}

					for (uint32_t i = 1; i < p0.degree; ++i)
					{
						mergedIndices.push_back(rotatedVertices0[i]);
						mergedVertices.push_back(mVertices[rotatedVertices0[i]]);
					}

					// Test merged vertices for convexity
					if (!MathsUtils::polygonIsConvex(mergedVertices))
					{
						continue;
					}

					// Calculate area of polygon
					float area = polygonArea(mergedVertices);

					if (area > maxArea)
					{
						maxArea = area;
						maxIndex = edgeIndex;

						maxVertexCount = (uint32_t)mergedVertices.size();
						memcpy(maxVertexIndices, &mergedIndices[0], maxVertexCount * sizeof(VertexIndex));
					}
				}

				if (maxIndex != NoIndex)
				{
					edgesToRemove = true;

					// Merge polygons
					PolygonIndex targetIndex = (PolygonIndex)mEdges[maxIndex].p[0];
					PolygonIndex sourceIndex = (PolygonIndex)mEdges[maxIndex].p[1];
					auto& target = mPolygons[targetIndex];
					auto& source = mPolygons[sourceIndex];

					// Set target vertex indices
					target.degree = maxVertexCount;
					memcpy(target.vertices, &maxVertexIndices[0], maxVertexCount * sizeof(VertexIndex));

					// Merge neighbours
					set<PolygonIndex> mergedNeighbours;
					for (uint32_t i = 0; i < target.numNeighbours; ++i)
					{
						mergedNeighbours.insert(target.neighbours[i]);
					}
					for (uint32_t i = 0; i < source.numNeighbours; ++i)
					{
						mergedNeighbours.insert(source.neighbours[i]);
					}

					// Don't include polygons being merged.
					mergedNeighbours.erase(targetIndex);
					mergedNeighbours.erase(sourceIndex);

					target.numNeighbours = 0;
					for (auto neighbour: mergedNeighbours)
					{
						target.neighbours[target.numNeighbours++] = neighbour;
					}

					// Delete joining edge
					mEdges[maxIndex].p[0] = mEdges[maxIndex].p[1] = mEdges[maxIndex].v[0] = mEdges[maxIndex].v[1] = NoIndex;

					// Update edges that reference source
					for (EdgeIndex i = 0; i < mEdges.size(); ++i)
					{
						if (i == (EdgeIndex)maxIndex)
						{
							continue;
						}

						auto& edge = mEdges[i];

						if (edge.p[0] == (PolygonIndex)sourceIndex)
						{
							edge.p[0] = (PossiblePolygonIndex)targetIndex;

							// Update neighbour
							if (edge.p[1] != NoIndex)
							{
								auto& neighbourPoly = mPolygons[edge.p[1]];
								for (uint32_t j = 0; j < neighbourPoly.numNeighbours; ++j)
								{
									if (neighbourPoly.neighbours[j] == sourceIndex)
									{
										neighbourPoly.neighbours[j] = targetIndex;
									}
								}
							}
						}

						if (edge.p[1] == (PossiblePolygonIndex)sourceIndex)
						{
							edge.p[1] = (PossiblePolygonIndex)targetIndex;

							// Update neighbour
							if (edge.p[0] != NoIndex)
							{
								auto& neighbourPoly = mPolygons[edge.p[0]];
								for (uint32_t j = 0; j < neighbourPoly.numNeighbours; ++j)
								{
									if (neighbourPoly.neighbours[j] == sourceIndex)
									{
										neighbourPoly.neighbours[j] = targetIndex;
									}
								}
							}
						}
					}

					// Delete source
					source.degree = 0;
				}
			}

			// Remove deleted edges and polygons.  Update edge references to polygons.
			vector<Polygon> newPolygons;
			map<PolygonIndex, PolygonIndex> polygonMap;

			for (PolygonIndex i = 0; i < mPolygons.size(); ++i)
			{
				auto const& polygon = mPolygons[i];

				if (polygon.degree > 0)
				{
					polygonMap[i] = (PolygonIndex)newPolygons.size();
					newPolygons.push_back(polygon);
				}
			}

			mPolygons = newPolygons;

			// Update polygon neighbours
			for (PolygonIndex i = 0; i < mPolygons.size(); ++i)
			{
				auto& polygon = mPolygons[i];

				for (uint32_t j = 0; j < polygon.numNeighbours; ++j)
				{
					polygon.neighbours[j] = polygonMap[polygon.neighbours[j]];
				}
			}

			// Update edges
			mVertexToEdgeLookup.clear();
			mPolygonToEdgeLookup.clear();

			vector<Edge> newEdges;

			for (auto& edge: mEdges)
			{
				if (edge.v[0] != NoIndex && edge.v[1] != NoIndex)
				{
					if (edge.p[0] >= 0)
					{
						edge.p[0] = (PossiblePolygonIndex)polygonMap[(PolygonIndex)edge.p[0]];
					}
					if (edge.p[1] >= 0)
					{
						edge.p[1] = (PossiblePolygonIndex)polygonMap[(PolygonIndex)edge.p[1]];
					}

					// Rebuild edge lookups
					EdgeIndex edgeIndex = (EdgeIndex)newEdges.size();
					mVertexToEdgeLookup[(edge.v[0] << 16) + edge.v[1]] = edgeIndex;
					mVertexToEdgeLookup[(edge.v[1] << 16) + edge.v[0]] = edgeIndex;

					mPolygonToEdgeLookup[(edge.p[0] << 16) + edge.p[1]] = edgeIndex;
					mPolygonToEdgeLookup[(edge.p[1] << 16) + edge.p[0]] = edgeIndex;

					newEdges.push_back(edge);
				}
			}

			mEdges = newEdges;
		}

		Vector2 const& ConvexPolygonisation::getVertex(VertexIndex index) const
		{
			return mVertices[index];
		}

		ConvexPolygonisation::Edge const& ConvexPolygonisation::getEdge(EdgeIndex index) const
		{
			return mEdges[index];
		}

		vector<ConvexPolygonisation::Polygon> const& ConvexPolygonisation::getPolygons() const
		{
			return mPolygons;
		}

		uint32_t ConvexPolygonisation::getNumPolygons() const
		{
			return (uint32_t)mPolygons.size();
		}

		int32_t ConvexPolygonisation::getContainingPolygon(float x, float y) const
		{
			BoundingCircle area;
			area.setPosition(x, y);
			area.setRadius(1);

			AccelerationGrid::IndexCollection indices;
			mPolygonAccelerationGrid->getCandidateItemsInBoundingArea(area, indices);
			for (auto i: indices)
			{
				auto const& polygon = mPolygons[i];

				for (uint32_t j = 0; j < polygon.degree - 2; ++j)
				{
					auto const& v0 = mVertices[polygon.vertices[0]];
					auto const& v1 = mVertices[polygon.vertices[j + 1]];
					auto const& v2 = mVertices[polygon.vertices[j + 2]];

					if (MathsUtils::pointInTriangle(Vector2(x, y), v0, v1, v2))
					{
						return i;
					}
				}
			}

			return -1;
		}

		int32_t ConvexPolygonisation::getContainingPolygon(Vector2 const& position) const
		{
			return getContainingPolygon(position.x, position.y);
		}

	} // wayfinder
} // WP_NAMESPACE
