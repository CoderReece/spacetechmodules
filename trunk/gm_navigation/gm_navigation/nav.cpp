/*
    gm_navigation
    By Spacetech
*/

#include "nav.h"
#include "ILuaModuleManager.h"
#include "tier0/memdbgon.h"

extern IEngineTrace *enginetrace;
extern IFileSystem *filesystem;

#ifndef USE_BOOST_THREADS
extern IThreadPool *threadPool;
#endif

extern FileHandle_t fh;
extern FILE* pDebugFile;

class GMOD_TraceFilter : public CTraceFilter
{
public:
	GMOD_TraceFilter()
	{
	}

	virtual bool ShouldHitEntity(IHandleEntity *pHandleEntity, int contentsMask)
	{
#ifdef LUA_SERVER
		IServerUnknown *pUnk = (IServerUnknown*)pHandleEntity;
#else
		IClientUnknown *pUnk = (IClientUnknown*)pHandleEntity;
#endif
		pCollide = pUnk->GetCollideable();
		return CheckCollisionGroup(pCollide->GetCollisionGroup());
	}

	bool CheckCollisionGroup(int collisionGroup)
	{
		if(collisionGroup == COLLISION_GROUP_PLAYER)
		{
			return false;
		}
		return true;
	}

private:
	ICollideable *pCollide;
};

inline void UTIL_TraceLine_GMOD(const Vector& vecAbsStart, const Vector& vecAbsEnd, unsigned int mask, trace_t *ptr)
{
	if(enginetrace == NULL)
	{
		return;
	}
	Ray_t ray;
	ray.Init(vecAbsStart, vecAbsEnd);
	GMOD_TraceFilter traceFilter;
	enginetrace->TraceRay(ray, mask, &traceFilter, ptr);
}

inline void UTIL_TraceHull_GMOD(const Vector& vecAbsStart, const Vector& vecAbsEnd, const Vector& vecMins, const Vector& vecMaxs, unsigned int mask, trace_t *ptr)
{
	if(enginetrace == NULL)
	{
		return;
	}
	Ray_t ray;
	ray.Init(vecAbsStart, vecAbsEnd, vecMins, vecMaxs);
	GMOD_TraceFilter traceFilter;
	enginetrace->TraceRay(ray, mask, &traceFilter, ptr);
}

Nav::Nav(int GridSize)
{
	heuristic = HEURISTIC_MANHATTAN;

	nodeStart = NULL;
	nodeEnd = NULL;

	generated = false;
	generating = false;
	generationMaxDistance = -1;

	mask = MASK_PLAYERSOLID_BRUSHONLY;

	SetDiagonal(false);
	SetGridSize(GridSize);

	nodes.EnsureCapacity(10240);
	groundSeedList.EnsureCapacity(16);
	airSeedList.EnsureCapacity(16);

#ifdef FILEBUG
	FILEBUG_WRITE("Created Nav\n");
#endif
}

Nav::~Nav()
{
#ifdef FILEBUG
	FILEBUG_WRITE("Deconstructing Nav\n");
#endif
	
	Msg("Deconstructing Nav!?\n");

#ifdef FILEBUG
	FILEBUG_WRITE("Freeing nodes\n");
#endif

	nodes.PurgeAndDeleteElements();
	//opened.PurgeAndDeleteElements();
   // closed.PurgeAndDeleteElements();

#ifdef FILEBUG
	FILEBUG_WRITE("Freed nodes\n");
#endif
}

NavDirType Nav::OppositeDirection(NavDirType Dir)
{
	switch(Dir)
	{
		case NORTH: return SOUTH;
		case SOUTH: return NORTH;
		case EAST:	return WEST;
		case WEST:	return EAST;
		case NORTHEAST: return SOUTHWEST;
		case NORTHWEST: return SOUTHEAST;
		case SOUTHEAST:	return NORTHWEST;
		case SOUTHWEST: return NORTHEAST;
		case UP: return DOWN;
		case DOWN: return UP;
		case LEFT:	return RIGHT;
		case RIGHT:	return LEFT;
		case FORWARD: return BACKWARD;
		case BACKWARD: return FORWARD;
	}

	return NORTH;
}

void Nav::SetGridSize(int GridSize)
{
	generationStepSize = GridSize;
	vecAddOffset = Vector(0, 0, GridSize / 2);
}

Node *Nav::GetNode(const Vector &Pos)
{
	const float Tolerance = 0.45f * generationStepSize;

	Node *Node;
	for(int i = 0; i < nodes.Count(); i++)
	{
		Node = nodes[i];
		float dx = fabs(Node->vecPos.x - Pos.x);
		float dy = fabs(Node->vecPos.y - Pos.y);
		float dz = fabs(Node->vecPos.z - Pos.z);
		if(dx < Tolerance && dy < Tolerance && dz < Tolerance)
		{
			return Node;
		}
	}

	return NULL;
}

Node *Nav::GetNodeByID(int ID)
{
	if(!nodes.IsValidIndex(ID))
	{
		return NULL;
	}
	return nodes.Element(ID);
}

Node *Nav::AddNode(const Vector &Pos, const Vector &Normal, NavDirType Dir, Node *Source)
{
	if(generationMaxDistance > 0)
	{
		if(vecOrigin.DistTo(Pos) > generationMaxDistance)
		{
			return NULL;
		}
	}

	// check if a node exists at this location
	Node *node = GetNode(Pos);
	
	// if no node exists, create one
	bool UseNew = false;
	if(node == NULL)
	{
		UseNew = true;
		node = new Node(Pos, Normal, Source);
		node->SetID(nodes.AddToTail(node));
#ifdef FILEBUG
		FILEBUG_WRITE("Adding Node <%f, %f>\n", Pos.x, Pos.y);
#endif
	}
	else
	{
#ifdef FILEBUG
		FILEBUG_WRITE("Using Existing Node <%f, %f>\n", Pos.x, Pos.y);
#endif
	}

	if(Source != NULL)
	{
		// connect source node to new node
		Source->ConnectTo(node, Dir);

		node->ConnectTo(Source, OppositeDirection(Dir));
		node->MarkAsVisited(OppositeDirection(Dir));

		if(UseNew)
		{
			// new node becomes current node
			currentNode = node;
		}
	}

	/* hmmmmm
	// optimization: if deltaZ changes very little, assume connection is commutative
	const float zTolerance = 50.0f;
	if(fabs(Source->GetPosition()->z - Pos.z) < zTolerance)
	{
		Node->ConnectTo(Source, OppositeDirection(Dir));
		Node->MarkAsVisited(OppositeDirection(Dir));
	}
	*/

	return node;
}

void Nav::RemoveNode(Node *node)
{
	Node *Connection;
	for(int Dir = NORTH; Dir < NUM_DIRECTIONS_MAX; Dir++)
	{
		Connection = node->GetConnectedNode((NavDirType)Dir);
		if(Connection != NULL)
		{
			Connection->ConnectTo(NULL, OppositeDirection((NavDirType)Dir));
		}
	}

	nodes.Remove(node->GetID());

	// Update all node ids, removing a element from a utlvector will shift all the elements positions
	for(int i = 0; i < nodes.Count(); i++)
	{
		nodes[i]->SetID(i);
	}
}

int Nav::GetGridSize()
{
	return generationStepSize;
}

bool Nav::IsGenerated()
{
	return generated;
}

float Nav::SnapToGrid(float x)
{
	return Round(x, GetGridSize());
}

Vector Nav::SnapToGrid(const Vector& in, bool snapX, bool snapY)
{
	int scale = GetGridSize();

	Vector out(in);

	if(snapX)
	{
		out.x = Round(in.x, scale);
	}

	if(snapY)
	{
		out.y = Round(in.y, scale);
	}

	return out;
}

float Nav::Round(float Val, float Unit)
{
	Val = Val + ((Val < 0.0f) ? -Unit*0.5f : Unit*0.5f);
	return (float)(Unit * ((int)Val) / (int)Unit);
}

bool Nav::GetGroundHeight(const Vector &pos, float *height, Vector *normal)
{
	Vector to;
	to.x = pos.x;
	to.y = pos.y;
	to.z = pos.z - 9999.9f;

	float offset;
	Vector from;
	trace_t result;

	const float maxOffset = 100.0f;
	const float inc = 10.0f;

	struct GroundLayerInfo
	{
		float ground;
		Vector normal;
	}
	layer[ MAX_GROUND_LAYERS ];
	int layerCount = 0;

	for( offset = 1.0f; offset < maxOffset; offset += inc )
	{
		from = pos + Vector( 0, 0, offset );

		//GMU->TraceLine( from, to, mask, &result);
		UTIL_TraceLine_GMOD(from, to, mask, &result);

		// if the trace came down thru a door, ignore the door and try again
		// also ignore breakable floors

		if (result.startsolid == false)
		{
			// if we didnt start inside a solid area, the trace hit a ground layer

			// if this is a new ground layer, add it to the set
			if (layerCount == 0 || result.endpos.z > layer[ layerCount-1 ].ground)
			{
				layer[ layerCount ].ground = result.endpos.z;
				if (result.plane.normal.IsZero())
					layer[ layerCount ].normal = Vector( 0, 0, 1 );
				else
					layer[ layerCount ].normal = result.plane.normal;

				++layerCount;
						
				if (layerCount == MAX_GROUND_LAYERS)
					break;
			}
		}
	}

	if (layerCount == 0)
		return false;

	// find the lowest layer that allows a player to stand or crouch upon it
	int i;
	for( i=0; i<layerCount-1; ++i )
	{
		if (layer[i+1].ground - layer[i].ground >= HumanHeight)
			break;		
	}

	*height = layer[ i ].ground;

	if (normal)
		*normal = layer[ i ].normal;

	return true;
}

#ifdef USE_BOOST_THREADS
void Nav::GenerateQueue(JobInfo_t *info)
{
	if(generating)
	{
		return;
	}

#ifdef FILEBUG
	FILEBUG_WRITE("GenerateQueue 1\n");
#endif

	ResetGeneration();

#ifdef FILEBUG
	FILEBUG_WRITE("GenerateQueue 2\n");
#endif

	info->thread = boost::thread(&Nav::FullGeneration, this, &info->abort);  

#ifdef FILEBUG
	FILEBUG_WRITE("Created Job\n");
#endif
}
#else
void Nav::GenerateQueue(JobInfo_t *info)
{
	if(generating)
	{
		return;
	}

#ifdef FILEBUG
	FILEBUG_WRITE("GenerateQueue 1\n");
#endif

	ResetGeneration();

#ifdef FILEBUG
	FILEBUG_WRITE("GenerateQueue 2\n");
#endif

	info->job = threadPool->QueueCall(this, &Nav::FullGeneration, &info->abort);

#ifdef FILEBUG
	FILEBUG_WRITE("Created Job\n");
#endif
}
#endif

void Nav::FullGeneration(bool *abort)
{
#ifdef FILEBUG
	FILEBUG_WRITE("FullGeneration nodeStart\n");
#endif

	while(!SampleStep() && (abort == NULL || *abort == false))
	{
	}

#ifdef FILEBUG
	FILEBUG_WRITE("FullGeneration nodeEnd\n");
#endif
}

void Nav::ResetGeneration()
{
	groundSeedIndex = 0;
	airSeedIndex = 0;
	currentNode = NULL;

	generating = true;
	generated = false;
	generatingGround = true;

	nodes.RemoveAll();
}

void Nav::SetupMaxDistance(const Vector &Pos, int MaxDistance)
{
	vecOrigin = Pos;
	generationMaxDistance = MaxDistance;
}

void Nav::AddGroundSeed(const Vector &pos, const Vector &normal)
{
	GroundSeedSpot seed;
	seed.pos = SnapToGrid(pos);
	seed.normal = normal;
	groundSeedList.AddToTail(seed);
}

void Nav::AddAirSeed(const Vector &pos)
{
	AirSeedSpot seed;
	seed.pos = SnapToGrid(pos);
	airSeedList.AddToTail(seed);
}

void Nav::ClearGroundSeeds()
{
	groundSeedList.RemoveAll();
}

void Nav::ClearAirSeeds()
{
	airSeedList.RemoveAll();
}

Node* Nav::GetNextGroundSeedNode()
{
#ifdef FILEBUG
	FILEBUG_WRITE("GetNextGroundSeedNode: %i\n", groundSeedIndex);
#endif

	if(groundSeedIndex == -1)
	{
#ifdef FILEBUG
		FILEBUG_WRITE("Invalid Seed Index 1\n");
#endif
		return NULL;
	}

	if(!groundSeedList.IsValidIndex(groundSeedIndex))
	{
#ifdef FILEBUG
		FILEBUG_WRITE("Invalid Seed Index 2\n");
#endif
		return NULL;
	}

#ifdef FILEBUG
	FILEBUG_WRITE("GetNextGroundSeedNode: Continuing 1\n");
#endif

	GroundSeedSpot spot = groundSeedList.Element(groundSeedIndex);

#ifdef FILEBUG
	FILEBUG_WRITE("GetNextGroundSeedNode: Continuing 2\n");
#endif

	groundSeedIndex = groundSeedList.Next(groundSeedIndex);

#ifdef FILEBUG
	FILEBUG_WRITE("GetNextGroundSeedNode: Continuing 3\n");
#endif

	if(GetNode(spot.pos) == NULL)
	{
		if(generationMaxDistance > 0)
		{
			if(vecOrigin.DistTo(spot.pos) > generationMaxDistance)
			{
#ifdef FILEBUG
				FILEBUG_WRITE("GetNextGroundSeedNode: Skipping Seed\n");
#endif
				return GetNextGroundSeedNode();
			}
		}
#ifdef FILEBUG
		FILEBUG_WRITE("GetNextGroundSeedNode: Adding Node\n");
#endif
		Node *node = new Node(spot.pos, spot.normal, NULL);
		node->SetID(nodes.AddToTail(node));
		return node;
	}

#ifdef FILEBUG
	FILEBUG_WRITE("GetNextWalkableSeedNode: Next Seed\n");
#endif

	return GetNextGroundSeedNode();
}

Node *Nav::GetNextAirSeedNode()
{
	if(airSeedIndex == -1)
	{
		return NULL;
	}

	if(!airSeedList.IsValidIndex(airSeedIndex))
	{
		return NULL;
	}

	AirSeedSpot spot = airSeedList.Element(airSeedIndex);

	airSeedIndex = airSeedList.Next(airSeedIndex);

	if(GetNode(spot.pos) == NULL)
	{
		if(generationMaxDistance > 0)
		{
			if(vecOrigin.DistTo(spot.pos) > generationMaxDistance)
			{
				return GetNextAirSeedNode();
			}
		}
		Node *node = new Node(spot.pos, vector_origin, NULL);
		node->SetID(nodes.AddToTail(node));
		return node;
	}

	return GetNextAirSeedNode();
}

bool Nav::SampleStep()
{
	if(IsGenerated())
	{
		return true;
	}

	if(!generating)
	{
		return true;
	}

	// take a ground step
	while(generatingGround)
	{
		if(currentNode == NULL)
		{
			currentNode = GetNextGroundSeedNode();

			if(currentNode == NULL)
			{
				generatingGround = false;
				break;
			}
		}

		//
		// Take a step from this node
		//

#ifdef FILEBUG
		FILEBUG_WRITE("Stepping From Node\n");
#endif
		for(int dir = NORTH; dir < numDirections; dir++)
		{
#ifdef FILEBUG
			FILEBUG_WRITE("Checking Direction: %i\n", dir);
#endif
			if(!currentNode->HasVisited((NavDirType)dir))
			{
				// have not searched in this direction yet

#ifdef FILEBUG
				FILEBUG_WRITE("Unsearched Direction: %i\n", dir);
#endif

				// start at current node position
				Vector Pos = *currentNode->GetPosition();

#ifdef FILEBUG
				FILEBUG_WRITE("1 <%f, %f>\n", Pos.x, Pos.y);
#endif
				switch(dir)
				{
					case NORTH:	Pos.y += generationStepSize; break;
					case SOUTH:	Pos.y -= generationStepSize; break;
					case EAST:	Pos.x += generationStepSize; break;
					case WEST:	Pos.x -= generationStepSize; break;
					case NORTHEAST:	Pos.x += generationStepSize; Pos.y += generationStepSize; break;
					case NORTHWEST:	Pos.x -= generationStepSize; Pos.y += generationStepSize; break;
					case SOUTHEAST: Pos.x += generationStepSize; Pos.y -= generationStepSize; break;
					case SOUTHWEST:	Pos.x -= generationStepSize; Pos.y -= generationStepSize; break;
				}

#ifdef FILEBUG
				FILEBUG_WRITE("2 <%f, %f>\n", Pos.x, Pos.y);
#endif

				generationDir = (NavDirType)dir;

				// mark direction as visited
				currentNode->MarkAsVisited(generationDir);

				// test if we can move to new position
				Vector to;

				// modify position to account for change in ground level during step
				to.x = Pos.x;
				to.y = Pos.y;

				Vector toNormal;

				if(GetGroundHeight(Pos, &to.z, &toNormal) == false)
				{
#ifdef FILEBUG
					FILEBUG_WRITE("Ground Height Fail\n");
#endif
					return false;
				}

				Vector from = *currentNode->GetPosition();

				Vector fromOrigin = from + vecAddOffset;
				Vector toOrigin = to + vecAddOffset;

				trace_t result;

				//GMU->TraceLine(fromOrigin, toOrigin, mask, &result);
				UTIL_TraceLine_GMOD(fromOrigin, toOrigin, mask, &result);

				bool walkable;

				if(result.fraction == 1.0f && !result.startsolid)
				{
					// the trace didnt hit anything - clear

					float toGround = to.z;
					float fromGround = from.z;

					float epsilon = 0.1f;

					// check if ledge is too high to reach or will cause us to fall to our death
					// Using generationStepSize instead of JumpCrouchHeight so that stairs will work with different grid sizes
					if(toGround - fromGround > generationStepSize + epsilon || fromGround - toGround > DeathDrop)
					{
						walkable = false;
#ifdef FILEBUG
						FILEBUG_WRITE("Bad Ledge\n");
#endif
					}
					else
					{
						// check surface normals along this step to see if we would cross any impassable slopes
						Vector delta = to - from;
						const float inc = 2.0f;
						float along = inc;
						bool done = false;
						float ground;
						Vector normal;

						walkable = true;

						while(!done)
						{
							Vector p;

							// need to guarantee that we test the exact edges
							if(along >= generationStepSize)
							{
								p = to;
								done = true;
							}
							else
							{
								p = from + delta * (along / generationStepSize);
							}

							if(GetGroundHeight(p, &ground, &normal) == false)
							{
								walkable = false;
#ifdef FILEBUG
								FILEBUG_WRITE("Bad Node Path\n");
#endif
								break;
							}

							// check for maximum allowed slope
							if(normal.z < 0.65)
							{
								walkable = false;
#ifdef FILEBUG
								FILEBUG_WRITE("Slope\n");
#endif
								break;
							}

							along += inc;					
						}
					}
				}
				else // TraceLine hit something...
				{
					walkable = false;
#ifdef FILEBUG
					FILEBUG_WRITE("Hit Something\n");
#endif
				}

				if(walkable)
				{
#ifdef FILEBUG
					FILEBUG_WRITE("Walkable 1!\n");
#endif
					AddNode(to, toNormal, generationDir, currentNode);
#ifdef FILEBUG
					FILEBUG_WRITE("Walkable 2!\n");
#endif
				}

				return false;
			}
		}

		// all directions have been searched from this node - pop back to its parent and continue
		currentNode = currentNode->GetParent();
	}

	// Step through air
	while(true)
	{
		if(currentNode == NULL)
		{
			currentNode = GetNextAirSeedNode();
			//Msg("GetNextAirSeedNode\n");
			if(currentNode == NULL)
			{
				break;
			}
		}

		for(int dir = UP; dir < NUM_DIRECTIONS_MAX; dir++) // UP to the end!
		{
			if(!currentNode->HasVisited((NavDirType)dir))
			{
				// start at current node position
				Vector pos = *currentNode->GetPosition();

				switch(dir)
				{
					case UP:		pos.z += generationStepSize; break;
					case DOWN:		pos.z -= generationStepSize; break;
					case LEFT:		pos.x -= generationStepSize; break;
					case RIGHT:		pos.x += generationStepSize; break;
					case FORWARD:	pos.y += generationStepSize; break;
					case BACKWARD:	pos.y -= generationStepSize; break;
				}

				generationDir = (NavDirType)dir;
				currentNode->MarkAsVisited(generationDir);
				
				// Trace from the current node to the pos (Check if we hit anything)
				trace_t result;

				//GMU->TraceLine(*currentNode->GetPosition(), pos, mask, &result);
				UTIL_TraceLine_GMOD(*currentNode->GetPosition(), pos, mask, &result);

				if(!result.DidHit())
				{
					//Msg("added node\n");
					AddNode(pos, vector_origin, generationDir, currentNode);
				}

				//Msg("HasVisited: %d %d\n", currentNode->GetID(), dir);

				return false;
			}
		}

		// all directions have been searched from this node - pop back to its parent and continue
		currentNode = currentNode->GetParent();

		//Msg("back to parent\n");
	}

	generated = true;
	generating = false;

	return true;
}

bool Nav::Save(const char *Filename)
{
	if(generating)
	{
		return false;
	}

	char path[MAX_PATH];
	Q_snprintf(path, MAX_PATH, "garrysmod/%s", Filename);

	FILE *pFile = fopen(path, "w");
	if(pFile == NULL)
	{
		return false;
	}

	Node *node, *connection;

	int iNodeConnections = 0;
	int iNodeTotal = nodes.Count();

	// Save current nav file version
	fprintf(pFile, "GM_NAVIGATION\t%d\n", NAV_VERSION);

	////////////////
	// Nodes
	fprintf(pFile, "%d\n", iNodeTotal);

	for(int i = 0; i < iNodeTotal; i++)
	{
		node = nodes[i];

		fprintf(pFile, "%f\t%f\t%f\t%f\t%f\t%f\n", node->vecPos.x, node->vecPos.y, node->vecPos.z, node->vecNormal.x, node->vecNormal.y, node->vecNormal.z);

		for(int Dir = NORTH; Dir < NUM_DIRECTIONS_MAX; Dir++)
		{
			if(node->GetConnectedNode((NavDirType)Dir) != NULL)
			{
				iNodeConnections++;
			}
		}
	}
	////////////////

	////////////////
	// Connections
	fprintf(pFile, "%d\n", iNodeConnections);

	for(int i = 0; i < iNodeTotal; i++)
	{
		node = nodes[i];
		for(int Dir = NORTH; Dir < NUM_DIRECTIONS_MAX; Dir++)
		{
			connection = node->GetConnectedNode((NavDirType)Dir);
			if(connection != NULL)
			{
				fprintf(pFile, "%d\t%d\t%d\n", Dir, node->GetID(), connection->GetID());
			}
		}
	}
	////////////////

	fclose(pFile);

	return true;
}

bool Nav::Load(const char *Filename)
{
	if(generating)
	{
		return false;
	}

	char path[MAX_PATH];
	Q_snprintf(path, MAX_PATH, "garrysmod/%s", Filename);

	FILE *pFile = fopen(path , "r");
	if(pFile == NULL)
	{
		return false;
	}

	Node *node;

	nodes.RemoveAll();

	int fileVersion;
	fscanf(pFile, "GM_NAVIGATION\t%d\n", &fileVersion);

	////////////////
	// Nodes

	int iNodeTotal;
	fscanf(pFile, "%d\n", &iNodeTotal);

	for(int i = 0; i < iNodeTotal; i++)
	{
		Vector pos, normal;

		fscanf(pFile, "%f\t%f\t%f\t%f\t%f\t%f\n", &pos.x, &pos.y, &pos.z, &normal.x, &normal.y, &normal.z);

		node = new Node(pos, normal, NULL);

		node->SetID(nodes.AddToTail(node));
	}
	////////////////

	////////////////
	// Connections

	int iTotalConnections;
	fscanf(pFile, "%d\n", &iTotalConnections);

	int Dir, SrcID, DestID;
	for(int i = 0; i < iTotalConnections; i++)
	{
		fscanf(pFile, "%d\t%d\t%d\n", &Dir, &SrcID, &DestID);

		// Should never be a problem...
		if(Dir <= NUM_DIRECTIONS_MAX - 1 && DestID - 1 <= nodes.Count() && SrcID - 1 <= nodes.Count())
		{
			nodes[SrcID]->ConnectTo(nodes[DestID], (NavDirType)Dir);
		}
		else
		{
			//FILEBUG_WRITE("Invalid connection\n", SrcID);
		}
	}

	////////////////

	return true;
}

CUtlVector<Node*>& Nav::GetNodes()
{
	return nodes;
}

Node *Nav::GetClosestNode(const Vector &Pos)
{
	float fNodeDist;
	float fDistance = -1;
	
	Node *pNode = NULL;
	Node *pNodeClosest = NULL;

	for(int i = 0; i < nodes.Count(); i++)
	{
		pNode = nodes[i];
		fNodeDist = Pos.DistTo(pNode->vecPos);
		if(fDistance == -1 || fNodeDist < fDistance)
		{
			pNodeClosest = pNode;
			fDistance = fNodeDist;
		}
	}

	return pNodeClosest;
}

void Nav::Reset()
{
	Node *node;
	for(int i = 0; i < nodes.Count(); i++)
	{
		node = nodes[i];
		node->SetOpened(false);
		node->SetClosed(false);
	}
	opened.RemoveAll();
	closed.RemoveAll();
}

void Nav::AddOpenedNode(Node *node)
{
	node->SetOpened(true);
	opened.AddToTail(node);
}

void Nav::AddClosedNode(Node *node)
{
	node->SetClosed(true);
	bool Removed = opened.FindAndRemove(node);
	if(!Removed)
	{
		Msg("Failed to remove Node!?\n");
	}
}

float Nav::HeuristicDistance(const Vector *vecStartPos, const Vector *EndPos)
{
	if(heuristic == HEURISTIC_MANHATTAN)
	{
		return ManhattanDistance(vecStartPos, EndPos);
	}
	else if(heuristic == HEURISTIC_EUCLIDEAN)
	{
		return EuclideanDistance(vecStartPos, EndPos);
	}
	else if(heuristic == HEURISTIC_CUSTOM)
	{
		//gLua->PushReference(heuristicRef);
			//GMOD_PushVector(vecStartPos);
			//GMOD_PushVector(EndPos);
		//gLua->Call(2, 1);
		//return gLua->GetNumber(1);
	}
	return NULL;
}

float Nav::ManhattanDistance(const Vector *vecStartPos, const Vector *EndPos)
{
	return (abs(EndPos->x - vecStartPos->x) + abs(EndPos->y - vecStartPos->y) + abs(EndPos->z - vecStartPos->z));
}

// u clid e an
float Nav::EuclideanDistance(const Vector *vecStartPos, const Vector *EndPos)
{
	return sqrt(pow(EndPos->x - vecStartPos->x, 2) + pow(EndPos->y - vecStartPos->y, 2) + pow(EndPos->z - vecStartPos->z, 2));
}

Node *Nav::FindLowestF()
{
	float BestScoreF = NULL;
	Node *node, *winner = NULL;
	for(int i = 0; i < opened.Count(); i++)
	{
		node = opened[i];
		if(BestScoreF == NULL || node->GetScoreF() < BestScoreF)
		{
			winner = node;
			BestScoreF = node->GetScoreF();
		}
	}
	return winner;
}

#ifdef USE_BOOST_THREADS
void Nav::FindPathQueue(JobInfo_t *info)
{
	info->thread = boost::thread(&Nav::ExecuteFindPath, this, info, nodeStart, nodeEnd);  
}

#else
void Nav::FindPathQueue(JobInfo_t *info)
{
	info->job = threadPool->QueueCall(this, &Nav::ExecuteFindPath, info, nodeStart, nodeEnd);
}
#endif

void Nav::ExecuteFindPath(JobInfo_t *info, Node *start, Node *end)
{
	lock.Lock();
	Reset();

	info->foundPath = false;

	if(start == NULL || end == NULL || start == end)
	{
		lock.Unlock();
		return;
	}

	AddOpenedNode(start);
	start->SetStatus(NULL, HeuristicDistance(start->GetPosition(), end->GetPosition()), 0);

	float currentScoreG, scoreG;
	Node *current = NULL, *connection = NULL;
	
#ifdef FILEBUG
	FILEBUG_WRITE("Added Node\n");
#endif

	while(!info->abort)
	{
		current = FindLowestF();

		if(current == NULL)
		{
			break;
		}

		if(current == end)
		{
			info->foundPath = true;
			break;
		}
		else
		{
			AddClosedNode(current);

			currentScoreG = current->GetScoreG();

			for(int Dir = NORTH; Dir < NUM_DIRECTIONS_MAX; Dir++) // numDirections
			{
				connection = current->GetConnectedNode((NavDirType)Dir);

				if(connection == NULL)
				{
					continue;
				}

				if(connection->IsClosed() || connection->IsDisabled())
				{
					continue;
				}
				
				scoreG = currentScoreG + EuclideanDistance(current->GetPosition(), connection->GetPosition()); // dist_between here

				if(!connection->IsOpened() || scoreG < connection->GetScoreG())
				{
					if(info->hull)
					{
						trace_t result;
						UTIL_TraceHull_GMOD(*current->GetPosition(), *connection->GetPosition(), info->mins, info->maxs, mask, &result);
						if(result.DidHit())
						{
							continue;
						}
					}

					AddOpenedNode(connection);
					connection->SetStatus(current, scoreG + HeuristicDistance(connection->GetPosition(), end->GetPosition()), scoreG);
				}
			}
		}
	}

	while(true)
	{
		if(current == NULL)
		{
			break;
		}
		info->path.AddToHead(current);
		current = current->GetAStarParent();
	}

	lock.Unlock();
}

int Nav::GetMask()
{
	return mask;
}

void Nav::SetMask(int M)
{
	mask = M;
}

bool Nav::GetDiagonal()
{
	return bDiagonalMode;
}

void Nav::SetDiagonal(bool Diagonal)
{
	bDiagonalMode = Diagonal;
	if(bDiagonalMode)
	{
		numDirections = NUM_DIRECTIONS_DIAGONAL;
	}
	else
	{
		numDirections = NUM_DIRECTIONS;
	}
}

int Nav::GetNumDir()
{
	return numDirections;
}

int Nav::GetHeuristic()
{
	return heuristic;
}

Node *Nav::GetStart()
{
	return nodeStart;
}

Node *Nav::GetEnd()
{
	return nodeEnd;
}

void Nav::SetHeuristic(int H)
{
	heuristic = H;
}

void Nav::SetStart(Node *start)
{
	nodeStart = start;
}

void Nav::SetEnd(Node *end)
{
	nodeEnd = end;
}

#ifdef SASSILIZATION

void GMOD_PushVector(lua_State* L, Vector& vec);
ILuaObject* NewVectorObject(lua_State* L, Vector& vec);

bool IsTerritory(Node *node, NavDirType dir, int empire)
{
	Node *connected = node->GetConnectedNode(dir);
	if( !connected )
		return true;
	else
		return connected->GetScoreG() > 0 && connected->GetScoreF() == empire;
}

void Nav::AddConnection( lua_State* L, CUtlVector<Border*> &borders, Node *node, NavDirType nextDir, NavDirType prevDir )
{
	if(node->GetBorder() != NULL)
	{
		//Lua()->Error("ASSERT Failed: Node border was not null");
		return;
	}

	Node *nodeNext = node->GetConnectedNode(nextDir);
	Node *nodePrev = node->GetConnectedNode(prevDir);

	Border *border1 = NULL;
	if( nodeNext )
		border1 = nodeNext->GetBorder();
	Border *border2 = NULL;
	if( nodePrev )
		border2 = nodePrev->GetBorder();
	
	if( border1 == NULL && border2 == NULL )
	{
		//create new border;
		Border *border = new Border();
		border->head = node;
		border->tail = node;
#ifdef FILEBUG
		FILEBUG_WRITE("New Border: %d\n", node->GetID());
#endif
		node->SetBorder( border );
		borders.AddToTail( border );
		return;
	}

	if( border1 && border2 ) //merge
	{
		if( border1 != border2 )
		{
#ifdef FILEBUG
			if(border1->head != NULL && border1->tail != NULL && border2->head != NULL && border2->tail != NULL)
			{
				FILEBUG_WRITE("Mering Borders: border1(%d, %d) border2(%d, %d)\n", border1->head->GetID(), border1->tail->GetID(), border2->head->GetID(), border2->tail->GetID());
			}
#endif
			if( border1->head != border1->tail )
				border1->tail->SetBorder(NULL);
			border1->tail->SetPrev(node);
			node->SetNext(border1->tail);
			border1->tail = border2->tail;
			if( border2->head != border2->tail )
				border2->head->SetBorder(NULL);
			border2->head->SetNext(node);
			node->SetPrev(border2->head);
			border2->tail->SetBorder( border1 );
			border1->tail->SetBorder( border1 );
			borders.FindAndRemove( border2 ); //abitrarily remove border2
			delete border2;
		}
		else
		{
			//loop
			border1->tail->SetBorder(NULL);
			border1->head->SetBorder(NULL);
			node->SetBorder( border1 );
			node->SetNext(border1->tail);
			border1->tail->SetPrev(node);
			node->SetPrev(border1->head);
			border1->head->SetNext(node);
			border1->head = node;
			border1->tail = node;
		}
	} else if( border1 ) //connect 2 next
	{
#ifdef FILEBUG
		if(border1->head != NULL && border1->tail != NULL)
		{
			FILEBUG_WRITE("Connecting 1: border1(%d, %d)\n", border1->head->GetID(), border1->tail->GetID());
		}
#endif
		if( border1->head != border1->tail )
			border1->tail->SetBorder(NULL);
		border1->tail->SetPrev(node);
		node->SetNext(border1->tail);
		border1->tail = node;
		node->SetBorder(border1);
	}
	else //( border2 ) //connect to prev
	{
#ifdef FILEBUG
		if(border2->head != NULL && border2->tail != NULL)
		{
			FILEBUG_WRITE("Connecting 2: border2(%d, %d)\n", border2->head->GetID(), border2->tail->GetID());
		}
#endif
		if( border2->head != border2->tail )
			border2->head->SetBorder(NULL);
		border2->head->SetNext(node);
		node->SetPrev(border2->head);
		border2->head = node;
		node->SetBorder(border2);
	}
}

/**
 * Flood is used to determining territory borders of empires.
 * for each node, scoreF determines the owner of the node and
 * scoreG determines distance from influence.  Lower scoreG
 * wins the node if two empires compete.
 *
 */
void Nav::Flood(lua_State* L, CUtlLuaVector* pairs)
{
	lock.Lock();
	
	Reset();

	bool start = false;

	int count = 1;

	ILuaObject *ResultTable = Lua()->GetNewTable();
	ResultTable->Push();
	ResultTable->UnReference();

	// open all nodes in the given table
	for( int i = 0; i < pairs->Count(); i++ )
	{
		LuaKeyValue& entry = pairs->Element(i);

		if(entry.pValue->GetType() != Type::TABLE)
		{
			Lua()->DeleteLuaVector(pairs);
			Lua()->Error("Nav:Flood value is not a table.\n");
			return;
		}
		
		ILuaObject *t_node = entry.pValue->GetMember(1);
		ILuaObject *t_pid = entry.pValue->GetMember(2);
		ILuaObject *t_score = entry.pValue->GetMember(3);

		if( t_node->GetType() != NODE_TYPE)
		{
			Lua()->DeleteLuaVector(pairs);

			t_node->UnReference();
			t_pid->UnReference();
			t_score->UnReference();

			Lua()->Error("Nav:Flood table values incorrect, 1st\n");	
		}

		if( t_pid->GetType() != Type::NUMBER)
		{
			Lua()->DeleteLuaVector(pairs);

			t_node->UnReference();
			t_pid->UnReference();
			t_score->UnReference();

			Lua()->Error("Nav:Flood table values incorrect, 2nd\n");
		}

		if( t_score->GetType() != Type::NUMBER)
		{
			Lua()->DeleteLuaVector(pairs);

			t_node->UnReference();
			t_pid->UnReference();
			t_score->UnReference();

			Lua()->Error("Nav:Flood table values incorrect, 3rd\n");
		}

		Node* node = (Node*)t_node->GetUserData();
		if( node != NULL )
		{
			AddOpenedNode(node);
			node->SetStatus( NULL, (int)(t_pid->GetFloat()), t_score->GetFloat() );
			start = true;
		}

		t_node->UnReference();
		t_pid->UnReference();
		t_score->UnReference();
	}

	Lua()->DeleteLuaVector(pairs);

	//abort if we were given no nodes
	if(!start)
	{
		lock.Unlock();
		return;
	}
	
	CUtlVector<Node*> borderNodes;
	CUtlVector<Border*> borders;
	//CUtlDict<int, CUtlVector<Node*>> borders;

	while( opened.Count() > 0 )
	{
		Node *current = opened.Head();
		AddClosedNode(current);
		closed.AddToTail(current);
		
		//Msg("current: %f | %f\n", current->GetScoreF(), current->GetScoreG());

		for(int Dir = NORTH; Dir < NUM_DIRECTIONS; Dir++)
		{
			Node *connection = current->GetConnectedNode((NavDirType)Dir);

			if(connection == NULL)
			{
				continue;
			}

			if(connection->IsClosed())
			{
				continue;
			}
			
			float newScoreG = current->GetScoreG() + EuclideanDistance(current->GetPosition(), connection->GetPosition());
			if( newScoreG > 135 )
			{
				if( !connection->IsOpened() && Dir < NORTHEAST && current->GetScoreF() == 1 )
				{
					//connection->SetClosed( true );
					//borderNodes.AddToTail( current );
				}
			}
			else if( !connection->IsOpened() || newScoreG < connection->GetScoreG() )
			{
				connection->SetStatus( current, current->GetScoreF(), newScoreG );
				AddOpenedNode( connection );
				//Msg("added connection\n");
			}
		}
	}
	
	//FIND BORDER NODES, THIS ISN'T IDEAL!
	
	for( int i = 0; i < closed.Count(); i++ )
	{
		Node *current = closed.Element(i);

		for(int Dir = NORTH; Dir < NORTHEAST; Dir++)
		{
			Node *connection = current->GetConnectedNode((NavDirType)Dir);

			if(connection == NULL)
			{
				continue;
			}

			if(connection->GetScoreF() != current->GetScoreF())
			{
				if( !borderNodes.HasElement(current) )
				{
					borderNodes.AddToTail(current);
					//Msg("added border node\n");
				}
				break;
			}
		}
	}

	unsigned char borderflags = 0;
	static const unsigned char f_no = 0x01;
	static const unsigned char f_ne = 0x02;
	static const unsigned char f_ea = 0x04;
	static const unsigned char f_se = 0x08;
	static const unsigned char f_so = 0x10;
	static const unsigned char f_sw = 0x20;
	static const unsigned char f_we = 0x40;
	static const unsigned char f_nw = 0x80;
	Node *node;

	for(int i = 0; i < borderNodes.Count(); i++)
	{
		node = borderNodes[i];

#ifdef FILEBUG
		FILEBUG_WRITE("borderNodes %d: %d\n", i, node->GetID());
#endif

		borderflags = 0;
		int empire = (int)node->GetScoreF();
		if( IsTerritory(node, NORTH     , empire) ) borderflags |= f_no;
		if( IsTerritory(node, NORTHEAST , empire) ) borderflags |= f_ne;
		if( IsTerritory(node, EAST      , empire) ) borderflags |= f_ea;
		if( IsTerritory(node, SOUTHEAST , empire) ) borderflags |= f_se;
		if( IsTerritory(node, SOUTH     , empire) ) borderflags |= f_so;
		if( IsTerritory(node, SOUTHWEST , empire) ) borderflags |= f_sw;
		if( IsTerritory(node, WEST      , empire) ) borderflags |= f_we;
		if( IsTerritory(node, NORTHWEST , empire) ) borderflags |= f_nw;
		//flip the bits
		borderflags = ~borderflags;
		
		//NOTE: all connections must be counter-clockwise around the territory
		
		if( (borderflags & (f_no | f_ne | f_ea | f_so | f_we | f_nw)) == f_no )
		{
			AddConnection( L, borders, node, NORTHWEST, NORTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we)) == f_so )
		{
			AddConnection( L, borders, node, SOUTHEAST, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we)) == f_ea )
		{
			AddConnection( L, borders, node, NORTHEAST, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_so | f_sw | f_we | f_nw)) == f_we )
		{
			AddConnection( L, borders, node, SOUTHWEST, NORTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we)) == (f_ne | f_ea | f_se) )
		{
			AddConnection( L, borders, node, NORTH, SOUTH );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we)) == (f_se | f_so | f_sw) )
		{
			AddConnection( L, borders, node, EAST, WEST );
		}
		else if( (borderflags & (f_no | f_we | f_so | f_sw | f_we | f_nw)) == (f_sw | f_we | f_nw) )
		{
			AddConnection( L, borders, node, SOUTH, NORTH );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_we | f_nw)) == (f_no | f_ne | f_nw) )
		{
			AddConnection( L, borders, node, WEST, EAST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_we | f_nw)) == (f_no | f_ne) )
		{
			AddConnection( L, borders, node, NORTHWEST, EAST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we)) == (f_ne | f_ea) )
		{
			AddConnection( L, borders, node, NORTH, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we)) == (f_ea | f_se) )
		{
			AddConnection( L, borders, node, NORTHEAST, SOUTH );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we)) == (f_se | f_so) )
		{
			AddConnection( L, borders, node, EAST, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we)) == (f_so | f_sw) )
		{
			AddConnection( L, borders, node, SOUTHEAST, WEST );
		}
		else if( (borderflags & (f_no | f_ea | f_so | f_sw | f_we | f_nw)) == (f_sw | f_we) )
		{
			AddConnection( L, borders, node, SOUTH, NORTHWEST );
		}
		else if( (borderflags & (f_no | f_ea | f_so | f_sw | f_we | f_nw)) == (f_we | f_nw) )
		{
			AddConnection( L, borders, node, SOUTHWEST, NORTH );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_we | f_nw)) == (f_no | f_nw) )
		{
			AddConnection( L, borders, node, WEST, NORTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we | f_nw)) == (f_no | f_ea | f_nw) )
		{
			AddConnection( L, borders, node, WEST, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we | f_nw)) == (f_no | f_ea | f_se) )
		{
			AddConnection( L, borders, node, NORTHWEST, SOUTH );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_sw | f_we | f_nw)) == (f_ne | f_ea | f_so) )
		{
			AddConnection( L, borders, node, NORTH, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_sw | f_we | f_nw)) == (f_ea | f_so | f_sw) )
		{
			AddConnection( L, borders, node, NORTHEAST, WEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we | f_nw)) == (f_se | f_so | f_we) )
		{
			AddConnection( L, borders, node, EAST, NORTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we | f_nw)) == (f_so | f_we | f_nw) )
		{
			AddConnection( L, borders, node, SOUTHEAST, NORTH );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_sw | f_we)) == (f_no | f_sw | f_we) )
		{
			AddConnection( L, borders, node, SOUTH, NORTHEAST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_sw | f_we)) == (f_no | f_ne | f_we) )
		{
			AddConnection( L, borders, node, SOUTHWEST, EAST );
		} //inbetweens
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_sw | f_we)) == (f_no | f_we) )
		{
			AddConnection( L, borders, node, SOUTHWEST, NORTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_se | f_so | f_sw | f_we | f_nw)) == (f_no | f_ea) )
		{
			AddConnection( L, borders, node, NORTHWEST, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_sw | f_we | f_nw)) == (f_ea | f_so) )
		{
			AddConnection( L, borders, node, NORTHEAST, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_we | f_nw)) == (f_so | f_we) )
		{
			AddConnection( L, borders, node, SOUTHEAST, NORTHWEST );
		} //NORTHEAST EXCEPTION
		/*else if( (borderflags & (f_no | f_ea | f_so | f_se | f_we | f_nw)) == (f_se | f_we | f_nw) )
		{
			AddConnection( L, borders, node, NORTH, SOUTHEAST );
		}*/
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_we | f_nw)) == (f_se | f_we | f_nw) )
		{
			if( (borderflags & f_so) )
				AddConnection( L, borders, node, EAST, NORTH );
			//else
			//	AddConnection( L, borders, node, NORTH, SOUTHEAST );
		} //SOUTHEAST EXCEPTION
		/*else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_we | f_sw)) == (f_no | f_ne | f_sw) )
		{
			AddConnection( L, borders, node, EAST, SOUTHWEST );
		}*/
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so | f_sw)) == (f_no | f_ne | f_sw) )
		{
			if( (borderflags & f_we) )
				AddConnection( L, borders, node, SOUTH, EAST );
			//else
			//	AddConnection( L, borders, node, EAST, SOUTHWEST );
		} //SOUTHWEST EXCEPTION
		/*else if( (borderflags & (f_no | f_ea | f_se | f_so | f_we | f_nw)) == (f_ea | f_se | f_nw) )
		{
			AddConnection( L, borders, node, EAST, SOUTHWEST );
		}*/
		else if( (borderflags & (f_ea | f_se | f_so | f_sw | f_we | f_nw)) == (f_ea | f_se | f_nw) )
		{
			if( (borderflags & f_no) )
				AddConnection( L, borders, node, WEST, SOUTH );
			//else
			//	AddConnection( L, borders, node, SOUTH, NORTHWEST );
		} //NORTHWEST EXCEPTION
		/*else if( (borderflags & (f_no | f_ne | f_ea | f_so | f_sw | f_we)) == (f_ne | f_so | f_sw) )
		{
			AddConnection( L, borders, node, EAST, SOUTHWEST );
		}*/
		else if( (borderflags & (f_no | f_ne | f_so | f_sw | f_we | f_nw)) == (f_ne | f_so | f_sw) )
		{
			if( (borderflags & f_ea) )
				AddConnection( L, borders, node, NORTH, WEST );
			//else
			//	AddConnection( L, borders, node, NORTHEAST, WEST );
		} //CORNER CASES
		else if( (borderflags & (f_no | f_ea | f_so | f_we)) == (f_no | f_ea | f_so) )
		{
			AddConnection( L, borders, node, NORTHWEST, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ea | f_so | f_we)) == (f_no | f_ea | f_we) )
		{
			AddConnection( L, borders, node, SOUTHWEST, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_so | f_we)) == (f_no | f_so | f_we) )
		{
			AddConnection( L, borders, node, SOUTHEAST, NORTHEAST );
		}
		else if( (borderflags & (f_no | f_ea | f_so | f_we)) == (f_ea | f_so | f_we) )
		{
			AddConnection( L, borders, node, NORTHEAST, NORTHWEST );
		} 
		//THE FOLLOWING CASES NEED TO BE CORRECTED, BECAUSE THE CURRENT IMPLEMENTATION DOES NOT ALLOW NODES TO HAVE 2 BORDERS
		else if( borderflags == (f_no | f_so | f_sw | f_nw) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			AddConnection( L, borders, node->GetConnectedNode(EAST), SOUTH, NORTH );
			//AddConnection( L, borders, node, WEST, NORTHEAST );
			//AddConnection( L, borders, node, SOUTHEAST, WEST );
		}
		else if( borderflags == (f_no | f_ne | f_se | f_so) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			AddConnection( L, borders, node->GetConnectedNode(WEST), NORTH, SOUTH );
			//AddConnection( L, borders, node, NORTHWEST, EAST );
			//AddConnection( L, borders, node, EAST, SOUTHWEST );
		}
		else if( borderflags == (f_ne | f_ea | f_we | f_nw) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			AddConnection( L, borders, node->GetConnectedNode(SOUTH), WEST, EAST );
			//AddConnection( L, borders, node, NORTH, SOUTHEAST );
			//AddConnection( L, borders, node, SOUTHWEST, NORTH );
		}
		else if( borderflags == (f_ea | f_se | f_sw | f_we) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			AddConnection( L, borders, node->GetConnectedNode(NORTH), EAST, WEST );
			//AddConnection( L, borders, node, NORTHEAST, SOUTH );
			//AddConnection( L, borders, node, SOUTH, NORTHWEST );
		} //triangle corners
		else if( (borderflags & (f_ea | f_se | f_so | f_sw | f_we)) == (f_ea | f_se | f_we) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(NORTH) );
			//AddConnection( L, borders, node, SOUTHWEST, SOUTH );
			//if( (borderflags & (f_no | f_ne | f_nw)) == 0 )
			//	AddConnection( L, borders, node, NORTHEAST, NORTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so)) == (f_no | f_ne | f_so) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(WEST) );
			//AddConnection( L, borders, node, SOUTHEAST, EAST );
			//if( (borderflags & (f_sw | f_we | f_nw)) == 0 )
			//	AddConnection( L, borders, node, NORTHWEST, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_we | f_nw)) == (f_ea | f_we | f_nw) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(SOUTH) );
			//AddConnection( L, borders, node, NORTHEAST, NORTH );
			//if( (borderflags & (f_se | f_so | f_sw)) == 0 )
			//	AddConnection( L, borders, node, SOUTHWEST, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_so | f_sw | f_we | f_nw)) == (f_no | f_so | f_sw) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(EAST) );
			//AddConnection( L, borders, node, NORTHWEST, WEST );
			//if( (borderflags & (f_ne | f_ea | f_se)) == 0 )
			//	AddConnection( L, borders, node, SOUTHEAST, NORTHEAST );
		} //flipped triangle corners
		else if( (borderflags & (f_ea | f_se | f_so | f_sw | f_we)) == (f_ea | f_sw | f_we) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(NORTH) );
			//AddConnection( L, borders, node, SOUTH, SOUTHEAST );
			//if( (borderflags & (f_no | f_ne | f_nw)) == 0 )
			//	AddConnection( L, borders, node, NORTHEAST, NORTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_se | f_so)) == (f_no | f_se | f_so) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(WEST) );
			//AddConnection( L, borders, node, EAST, NORTHEAST );
			//if( (borderflags & (f_sw | f_we | f_nw)) == 0 )
			//	AddConnection( L, borders, node, NORTHWEST, SOUTHWEST );
		}
		else if( (borderflags & (f_no | f_ne | f_ea | f_we | f_nw)) == (f_ea | f_ne | f_we) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(SOUTH) );
			//AddConnection( L, borders, node, NORTH, NORTHWEST );
			//if( (borderflags & (f_se | f_so | f_sw)) == 0 )
			//	AddConnection( L, borders, node, SOUTHWEST, SOUTHEAST );
		}
		else if( (borderflags & (f_no | f_so | f_sw | f_we | f_nw)) == (f_no | f_so | f_nw) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(EAST) );
			//AddConnection( L, borders, node, WEST, SOUTHWEST );
			//if( (borderflags & (f_ne | f_ea | f_se)) == 0 )
			//	AddConnection( L, borders, node, SOUTHEAST, NORTHEAST );
		}
		else if( borderflags == (f_ea | f_we) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(NORTH) );
			borderNodes.AddToTail( node->GetConnectedNode(SOUTH) );
			//AddConnection( L, borders, node, SOUTHWEST, SOUTHEAST );
			//AddConnection( L, borders, node, NORTHEAST, NORTHWEST );
		}
		else if( borderflags == (f_no | f_so) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(EAST) );
			borderNodes.AddToTail( node->GetConnectedNode(WEST) );
			//AddConnection( L, borders, node, SOUTHEAST, NORTHEAST );
			//AddConnection( L, borders, node, NORTHWEST, SOUTHWEST );
		}
		else if( borderflags == (f_ne | f_so ) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(WEST) );
		}
		else if( borderflags == (f_ea | f_nw ) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(SOUTH) );
		}
		else if( borderflags == (f_no | f_sw ) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(EAST) );
		}
		else if( borderflags == (f_se | f_we ) )
		{
			node->SetStatus( NULL, 0, node->GetScoreG() );
			borderNodes.AddToTail( node->GetConnectedNode(NORTH) );
		}
		
	}

	//Msg("Borders: %d\n", borders.Count());

	//Build result table for Lua

	/*
	for(int i=0; i <= Lua()->Top(); i++)
	{
		Msg("%d Type: %s\n", i, Lua()->GetTypeName(Lua()->GetType(i)));
	}
	Msg("\n");*/

	for( int i = 0; i < borders.Count(); i++ )
	{
		ResultTable = Lua()->GetObject(3); //get the result table to survive past 510 calls

		Border *border = borders.Element(i);

		ILuaObject *tbl = Lua()->GetNewTable(); //create a new lua table for this border

		int nodeCount = 1;
		int empireID = 0;
		Node *start = border->tail;
		Node *current = start;

		while( current )
		{
			empireID = current->GetScoreF();

			ILuaObject* obj = NewVectorObject(L, (Vector&)*(current->GetPosition()));

			tbl->SetMember(nodeCount++, obj);
			obj->UnReference();

			current = current->GetNext();
			if( current == start )
			{
				ILuaObject* obj = NewVectorObject(L, (Vector&)*(current->GetPosition()));
				tbl->SetMember(nodeCount++, obj);
				obj->UnReference();
				break;
			}
		}

		tbl->SetMember("empireID", (float)empireID);

		ResultTable->SetMember((float)(count++), tbl);

		ResultTable->UnReference();

		tbl->UnReference();
	}

	borders.PurgeAndDeleteElements();

	lock.Unlock();
	/*
	for(int i=0; i <= Lua()->Top(); i++)
	{
		Msg("%d Type: %s\n", i, Lua()->GetTypeName(Lua()->GetType(i)));
	}*/
}

int Nav::GetTerritory(const Vector &pos)
{
	Node *node = GetClosestNode(pos);
	if(!node)
	{
		return 0;
	}
	return node->GetScoreF();
}
#endif
