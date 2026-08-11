#ifndef TKT_MAPGEN_H
#define TKT_MAPGEN_H

#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <engine/shared/datafile.h>
#include <engine/storage.h>
#include <game/mapitems.h>

// deterministic rng seeded by the map seed
struct CTKTRng
{
	uint64_t m_State;

	CTKTRng(uint64_t Seed) :
		m_State(Seed ? Seed : 0x9E3779B97F4A7C15ULL)
	{
	}

	uint32_t Next()
	{
		// xorshift64*
		m_State ^= m_State >> 12;
		m_State ^= m_State << 25;
		m_State ^= m_State >> 27;
		return (uint32_t)((m_State * 0x2545F4914F6CDD1DULL) >> 32);
	}

	int Below(int N)
	{
		return N > 0 ? Next() % N : 0;
	}
};

// grid cell types
enum
{
	TKT_TILE_AIR = 0,
	TKT_TILE_WALL, // game layer tile 1 (hookable)
	TKT_TILE_SPAWN, // game layer tile 192
	TKT_TILE_ITEM, // game layer tile 189
};

struct STKTMapData
{
	int m_Width;
	int m_Height;
	std::vector<unsigned char> m_aTiles; // m_Width * m_Height

	STKTMapData() :
		m_Width(0),
		m_Height(0)
	{
	}

	unsigned char &At(int x, int y)
	{
		return m_aTiles[y * m_Width + x];
	}

	unsigned char At(int x, int y) const
	{
		return m_aTiles[y * m_Width + x];
	}

	bool InBounds(int x, int y) const
	{
		return x >= 0 && y >= 0 && x < m_Width && y < m_Height;
	}

	// is the 3x3 area around (cx, cy) free of walls?
	bool IsClear3x3(int cx, int cy) const
	{
		for(int y = cy - 1; y <= cy + 1; ++y)
			for(int x = cx - 1; x <= cx + 1; ++x)
				if(!InBounds(x, y) || At(x, y) == TKT_TILE_WALL)
					return false;
		return true;
	}

	// carve a 3x3 free area around (cx, cy)
	void Carve3x3(int cx, int cy)
	{
		for(int y = cy - 1; y <= cy + 1; ++y)
			for(int x = cx - 1; x <= cx + 1; ++x)
				if(InBounds(x, y) && At(x, y) == TKT_TILE_WALL)
					At(x, y) = TKT_TILE_AIR;
	}
};

// flood fill the open space from a start tile, marking reachable tiles
static void TKTFloodFill(const STKTMapData &Map, int Start, std::vector<unsigned char> &Reached)
{
	Reached.assign(Map.m_Width * Map.m_Height, 0);
	std::vector<int> Queue;
	Queue.push_back(Start);
	Reached[Start] = 1;
	static const int s_Dx[4] = {0, 0, -1, 1};
	static const int s_Dy[4] = {-1, 1, 0, 0};
	for(size_t Head = 0; Head < Queue.size(); ++Head)
	{
		int Pos = Queue[Head];
		int x = Pos % Map.m_Width;
		int y = Pos / Map.m_Width;
		for(int i = 0; i < 4; ++i)
		{
			int nx = x + s_Dx[i];
			int ny = y + s_Dy[i];
			if(!Map.InBounds(nx, ny) || Map.At(nx, ny) == TKT_TILE_WALL)
				continue;
			int NPos = ny * Map.m_Width + nx;
			if(!Reached[NPos])
			{
				Reached[NPos] = 1;
				Queue.push_back(NPos);
			}
		}
	}
}

// guarantee that every open space is reachable: flood fill from the first
// open tile, run a multi-source BFS from the reached area through all tiles
// (walls passable), then for every unreached open region carve a corridor
// along a parent chain. The carving is 1..2 tiles wide, sometimes opens a
// whole wall face and does not always take the shortest path, so the map
// gains extra bypasses
static void ConnectOpenSpaces(STKTMapData &Map, uint64_t Seed)
{
	CTKTRng Rng(Seed);

	int Start = -1;
	for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
		if(Map.m_aTiles[i] != TKT_TILE_WALL)
		{
			Start = i;
			break;
		}
	if(Start < 0)
		return;

	std::vector<unsigned char> Reached;
	TKTFloodFill(Map, Start, Reached);

	// multi-source BFS from the reached set through all tiles, with depth
	std::vector<int> Parent(Map.m_Width * Map.m_Height, -1);
	std::vector<int> Depth(Map.m_Width * Map.m_Height, -1);
	std::vector<int> Queue;
	for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
		if(Reached[i])
		{
			Parent[i] = i;
			Depth[i] = 0;
			Queue.push_back(i);
		}
	static const int s_Dx[4] = {0, 0, -1, 1};
	static const int s_Dy[4] = {-1, 1, 0, 0};
	for(size_t Head = 0; Head < Queue.size(); ++Head)
	{
		int Pos = Queue[Head];
		int x = Pos % Map.m_Width;
		int y = Pos / Map.m_Width;
		for(int i = 0; i < 4; ++i)
		{
			int nx = x + s_Dx[i];
			int ny = y + s_Dy[i];
			if(!Map.InBounds(nx, ny))
				continue;
			int NPos = ny * Map.m_Width + nx;
			if(Parent[NPos] == -1)
			{
				Parent[NPos] = Pos;
				Depth[NPos] = Depth[Pos] + 1;
				Queue.push_back(NPos);
			}
		}
	}

	// one corridor per unreached open region
	for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
	{
		if(Map.m_aTiles[i] == TKT_TILE_WALL || Reached[i])
			continue;

		// flood the region
		std::vector<unsigned char> Region;
		TKTFloodFill(Map, i, Region);

		// pick the connection tile: mostly the closest one, sometimes a
		// random region tile so the corridor is not always the shortest
		int Best = i;
		if(Rng.Below(100) < 30)
		{
			int Num = 0;
			for(int k = 0; k < Map.m_Width * Map.m_Height; ++k)
				if(Region[k])
					Num++;
			int Pick = Rng.Below(Num);
			for(int k = 0; k < Map.m_Width * Map.m_Height; ++k)
				if(Region[k] && Pick-- == 0)
				{
					Best = k;
					break;
				}
		}
		else
		{
			for(int k = 0; k < Map.m_Width * Map.m_Height; ++k)
				if(Region[k] && Depth[k] >= 0 && Depth[k] < Depth[Best])
					Best = k;
		}

		// carve the parent chain, 1..2 tiles wide; remember the last wall
		// tile carved (the crossing point closest to the reached area)
		bool Wide = Rng.Below(100) < 60;
		int LastWallX = -1, LastWallY = -1;
		int Cur = Best;
		int Guard = Map.m_Width * Map.m_Height;
		while(Parent[Cur] != -1 && Parent[Cur] != Cur && Guard-- > 0)
		{
			int x = Cur % Map.m_Width;
			int y = Cur / Map.m_Width;
			if(Map.At(x, y) == TKT_TILE_WALL)
			{
				Map.At(x, y) = TKT_TILE_AIR;
				LastWallX = x;
				LastWallY = y;
				if(Wide)
				{
					// carve a random perpendicular neighbour as well
					if(Rng.Below(2) && Map.InBounds(x + 1, y) && Map.At(x + 1, y) == TKT_TILE_WALL)
						Map.At(x + 1, y) = TKT_TILE_AIR;
					else if(Map.InBounds(x - 1, y) && Map.At(x - 1, y) == TKT_TILE_WALL)
						Map.At(x - 1, y) = TKT_TILE_AIR;
					if(Rng.Below(2) && Map.InBounds(x, y + 1) && Map.At(x, y + 1) == TKT_TILE_WALL)
						Map.At(x, y + 1) = TKT_TILE_AIR;
					else if(Map.InBounds(x, y - 1) && Map.At(x, y - 1) == TKT_TILE_WALL)
						Map.At(x, y - 1) = TKT_TILE_AIR;
				}
			}
			Cur = Parent[Cur];
		}

		// sometimes open a whole wall face: at the crossing point, extend
		// the carve along the wall run for a few more tiles
		if(Rng.Below(100) < 30 && LastWallX != -1)
		{
			bool RunH = (Map.InBounds(LastWallX + 1, LastWallY) && Map.At(LastWallX + 1, LastWallY) == TKT_TILE_WALL) ||
				(Map.InBounds(LastWallX - 1, LastWallY) && Map.At(LastWallX - 1, LastWallY) == TKT_TILE_WALL);
			bool RunV = (Map.InBounds(LastWallX, LastWallY + 1) && Map.At(LastWallX, LastWallY + 1) == TKT_TILE_WALL) ||
				(Map.InBounds(LastWallX, LastWallY - 1) && Map.At(LastWallX, LastWallY - 1) == TKT_TILE_WALL);
			if(RunH || RunV)
			{
				int DirX = 0, DirY = 0;
				if(RunH && (!RunV || Rng.Below(2)))
					DirX = Rng.Below(2) ? 1 : -1;
				else
					DirY = Rng.Below(2) ? 1 : -1;
				int Run = 2 + Rng.Below(4); // 2..5 more tiles of the wall
				for(int r = 1; r <= Run; ++r)
				{
					int nx = LastWallX + DirX * r;
					int ny = LastWallY + DirY * r;
					if(!Map.InBounds(nx, ny) || Map.At(nx, ny) != TKT_TILE_WALL)
						break;
					Map.At(nx, ny) = TKT_TILE_AIR;
				}
			}
		}

		// the whole region is now connected
		for(int k = 0; k < Map.m_Width * Map.m_Height; ++k)
			if(Region[k])
				Reached[k] = 1;
	}

	// extra bypasses: open a few wall tiles that separate two open areas,
	// creating a limited number of alternative connected side paths
	std::vector<int> BypassTiles;
	for(int y = 1; y < Map.m_Height - 1; ++y)
		for(int x = 1; x < Map.m_Width - 1; ++x)
		{
			if(Map.At(x, y) != TKT_TILE_WALL)
				continue;
			bool L = Map.InBounds(x - 1, y) && Map.At(x - 1, y) != TKT_TILE_WALL;
			bool R = Map.InBounds(x + 1, y) && Map.At(x + 1, y) != TKT_TILE_WALL;
			bool U = Map.InBounds(x, y - 1) && Map.At(x, y - 1) != TKT_TILE_WALL;
			bool D = Map.InBounds(x, y + 1) && Map.At(x, y + 1) != TKT_TILE_WALL;
			if((L && R) || (U && D))
				BypassTiles.push_back(y * Map.m_Width + x);
		}
	int NumBypass = 2 + Rng.Below(5); // 2..6 extra connections
	while(NumBypass-- > 0 && !BypassTiles.empty())
	{
		int Idx = Rng.Below((int)BypassTiles.size());
		Map.m_aTiles[BypassTiles[Idx]] = TKT_TILE_AIR;
		BypassTiles[Idx] = BypassTiles.back();
		BypassTiles.pop_back();
	}
}

// connect all wall chunks into a single mass: flood the walls from the map
// border, then for every isolated wall region add a wall bridge along the
// shortest path back to the main wall mass
static void ConnectWalls(STKTMapData &Map)
{
	if(Map.m_aTiles[0] != TKT_TILE_WALL)
		return;

	// flood the main wall mass (the one touching the top-left corner)
	std::vector<unsigned char> MainWall(Map.m_Width * Map.m_Height, 0);
	{
		std::vector<int> Queue;
		Queue.push_back(0);
		MainWall[0] = 1;
		static const int s_Dx[4] = {0, 0, -1, 1};
		static const int s_Dy[4] = {-1, 1, 0, 0};
		for(size_t Head = 0; Head < Queue.size(); ++Head)
		{
			int Pos = Queue[Head];
			int x = Pos % Map.m_Width;
			int y = Pos / Map.m_Width;
			for(int i = 0; i < 4; ++i)
			{
				int nx = x + s_Dx[i];
				int ny = y + s_Dy[i];
				if(!Map.InBounds(nx, ny) || Map.At(nx, ny) != TKT_TILE_WALL)
					continue;
				int NPos = ny * Map.m_Width + nx;
				if(!MainWall[NPos])
				{
					MainWall[NPos] = 1;
					Queue.push_back(NPos);
				}
			}
		}
	}

	// multi-source BFS from the main wall mass through all tiles (open passable)
	std::vector<int> Parent(Map.m_Width * Map.m_Height, -1);
	std::vector<int> Depth(Map.m_Width * Map.m_Height, -1);
	std::vector<int> Queue;
	for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
		if(MainWall[i])
		{
			Parent[i] = i;
			Depth[i] = 0;
			Queue.push_back(i);
		}
	static const int s_Dx[4] = {0, 0, -1, 1};
	static const int s_Dy[4] = {-1, 1, 0, 0};
	for(size_t Head = 0; Head < Queue.size(); ++Head)
	{
		int Pos = Queue[Head];
		int x = Pos % Map.m_Width;
		int y = Pos / Map.m_Width;
		for(int i = 0; i < 4; ++i)
		{
			int nx = x + s_Dx[i];
			int ny = y + s_Dy[i];
			if(!Map.InBounds(nx, ny))
				continue;
			int NPos = ny * Map.m_Width + nx;
			if(Parent[NPos] == -1)
			{
				Parent[NPos] = Pos;
				Depth[NPos] = Depth[Pos] + 1;
				Queue.push_back(NPos);
			}
		}
	}

	// bridge every isolated wall region once: flood the region, pick the
	// tile closest to the main wall mass and add walls along its chain
	for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
	{
		if(Map.m_aTiles[i] != TKT_TILE_WALL || MainWall[i])
			continue;

		// flood this isolated wall region
		std::vector<unsigned char> Region(Map.m_Width * Map.m_Height, 0);
		{
			std::vector<int> RQueue;
			RQueue.push_back(i);
			Region[i] = 1;
			for(size_t Head = 0; Head < RQueue.size(); ++Head)
			{
				int Pos = RQueue[Head];
				int x = Pos % Map.m_Width;
				int y = Pos / Map.m_Width;
				for(int d = 0; d < 4; ++d)
				{
					int nx = x + s_Dx[d];
					int ny = y + s_Dy[d];
					if(!Map.InBounds(nx, ny) || Map.At(nx, ny) != TKT_TILE_WALL)
						continue;
					int NPos = ny * Map.m_Width + nx;
					if(!Region[NPos])
					{
						Region[NPos] = 1;
						RQueue.push_back(NPos);
					}
				}
			}
		}

		// the region tile closest to the main wall mass
		int Best = i;
		for(int k = 0; k < Map.m_Width * Map.m_Height; ++k)
			if(Region[k] && Depth[k] >= 0 && Depth[k] < Depth[Best])
				Best = k;

		// add walls along its chain back to the main mass
		int Cur = Best;
		int Guard = Map.m_Width * Map.m_Height;
		while(Parent[Cur] != -1 && Parent[Cur] != Cur && Guard-- > 0)
		{
			int x = Cur % Map.m_Width;
			int y = Cur / Map.m_Width;
			if(Map.At(x, y) == TKT_TILE_AIR)
				Map.At(x, y) = TKT_TILE_WALL;
			Cur = Parent[Cur];
		}

		// the whole region is now part of the main mass
		for(int k = 0; k < Map.m_Width * Map.m_Height; ++k)
			if(Region[k])
				MainWall[k] = 1;
	}
}

// pick the width/height whose product is closest to the target area,
// taking the smallest width on ties (dimensions 50..200)
static void TKTMapSizeFromArea(int TargetArea, int &OutW, int &OutH)
{
	int BestDist = 0x7fffffff;
	OutW = 50;
	OutH = 50;
	for(int W = 50; W <= 200; ++W)
	{
		for(int H = 50; H <= 200; ++H)
		{
			int Area = W * H;
			int Dist = Area > TargetArea ? Area - TargetArea : TargetArea - Area;
			if(Dist < BestDist || (Dist == BestDist && W < OutW))
			{
				BestDist = Dist;
				OutW = W;
				OutH = H;
			}
		}
	}
}
static STKTMapData GenerateTKTMap(uint64_t Seed, int Width, int Height)
{
	CTKTRng Rng(Seed);

	STKTMapData Map;
	Map.m_Width = Width;
	Map.m_Height = Height;
	// start fully solid, the rooms and corridors are carved inside,
	// leaving at least a 2 tile wall border all around
	Map.m_aTiles.assign(Map.m_Width * Map.m_Height, TKT_TILE_WALL);

	const int Border = 2;
	const int CellSize = 12;
	const int CellsW = (Map.m_Width - Border * 2) / CellSize;
	const int CellsH = (Map.m_Height - Border * 2) / CellSize;

	auto CarveRect = [&](int X0, int Y0, int X1, int Y1) {
		for(int y = Y0; y <= Y1; ++y)
			for(int x = X0; x <= X1; ++x)
				if(Map.InBounds(x, y) && Map.At(x, y) == TKT_TILE_WALL)
					Map.At(x, y) = TKT_TILE_AIR;
	};

	if(CellsW > 0 && CellsH > 0)
	{
		// each cell gets a room aligned to the cell grid, so the walls
		// between the rooms form long straight segments of equal length
		std::vector<int> RoomX(CellsW * CellsH), RoomY(CellsW * CellsH);
		for(int cy = 0; cy < CellsH; ++cy)
		{
			for(int cx = 0; cx < CellsW; ++cx)
			{
				int Margin = 1 + Rng.Below(2); // 1..2 tile walls around each room
				int X = Border + cx * CellSize + Margin;
				int Y = Border + cy * CellSize + Margin;
				RoomX[cy * CellsW + cx] = X;
				RoomY[cy * CellsW + cx] = Y;
				CarveRect(X, Y, X + CellSize - Margin * 2 - 1, Y + CellSize - Margin * 2 - 1);
			}
		}

		// connect each room to its right and bottom neighbours with 1..2 wide corridors
		auto CarveCorridor = [&](int X0, int Y0, int X1, int Y1) {
			int Width2 = 1 + Rng.Below(2);
			if(X1 < X0 || Y1 < Y0)
			{
				int Tmp = X0;
				X0 = X1;
				X1 = Tmp;
				Tmp = Y0;
				Y0 = Y1;
				Y1 = Tmp;
			}
			for(int X = X0; X <= X1; ++X)
				CarveRect(X, Y0, X, Y0 + Width2 - 1);
			for(int Y = Y0; Y <= Y1; ++Y)
				CarveRect(X1, Y, X1, Y);
		};
		for(int cy = 0; cy < CellsH; ++cy)
		{
			for(int cx = 0; cx < CellsW; ++cx)
			{
				int Idx = cy * CellsW + cx;
				if(cx + 1 < CellsW)
				{
					int NIdx = cy * CellsW + cx + 1;
					CarveCorridor(RoomX[Idx] + CellSize / 2, RoomY[Idx] + CellSize / 2, RoomX[NIdx] + CellSize / 2, RoomY[NIdx] + CellSize / 2);
				}
				if(cy + 1 < CellsH)
				{
					int NIdx = (cy + 1) * CellsW + cx;
					CarveCorridor(RoomX[Idx] + CellSize / 2, RoomY[Idx] + CellSize / 2, RoomX[NIdx] + CellSize / 2, RoomY[NIdx] + CellSize / 2);
				}
			}
		}
	}

	// remove single wall blocks floating in the air
	for(int y = 1; y < Map.m_Height - 1; ++y)
		for(int x = 1; x < Map.m_Width - 1; ++x)
		{
			if(Map.At(x, y) != TKT_TILE_WALL)
				continue;
			if(Map.At(x - 1, y) == TKT_TILE_AIR && Map.At(x + 1, y) == TKT_TILE_AIR &&
				Map.At(x, y - 1) == TKT_TILE_AIR && Map.At(x, y + 1) == TKT_TILE_AIR)
				Map.At(x, y) = TKT_TILE_AIR;
		}

	// thin the wall side-branches: remove dead-end wall tiles, cascading
	for(int Pass = 0; Pass < 16; ++Pass)
	{
		bool Changed = false;
		for(int y = 1; y < Map.m_Height - 1; ++y)
			for(int x = 1; x < Map.m_Width - 1; ++x)
			{
				if(Map.At(x, y) != TKT_TILE_WALL)
					continue;
				int Neighbors = 0;
				if(Map.At(x - 1, y) == TKT_TILE_WALL)
					Neighbors++;
				if(Map.At(x + 1, y) == TKT_TILE_WALL)
					Neighbors++;
				if(Map.At(x, y - 1) == TKT_TILE_WALL)
					Neighbors++;
				if(Map.At(x, y + 1) == TKT_TILE_WALL)
					Neighbors++;
				if(Neighbors <= 1)
				{
					Map.At(x, y) = TKT_TILE_AIR;
					Changed = true;
				}
			}
		if(!Changed)
			break;
	}

	// evenly spaced spawn points: at most one per 5x5, only on already
	// clear 3x3 areas so no wall pockets are carved
	{
		const int Step = 9;
		const int MinX = Border + 2;
		const int MaxX = Map.m_Width - Border - 2;
		const int MinY = Border + 2;
		const int MaxY = Map.m_Height - Border - 2;
		for(int y = MinY; y <= MaxY; y += Step)
			for(int x = MinX; x <= MaxX; x += Step)
			{
				if(Map.IsClear3x3(x, y))
					Map.At(x, y) = TKT_TILE_SPAWN;
			}
	}

	// evenly spaced item spawn points: at most one per 3x3, only on already
	// clear 3x3 areas
	{
		const int Step = 5;
		const int MinX = Border + 2;
		const int MaxX = Map.m_Width - Border - 2;
		const int MinY = Border + 2;
		const int MaxY = Map.m_Height - Border - 2;
		for(int y = MinY; y <= MaxY; y += Step)
			for(int x = MinX; x <= MaxX; x += Step)
			{
				if(Map.At(x, y) == TKT_TILE_SPAWN)
					continue;
				if(Map.IsClear3x3(x, y))
					Map.At(x, y) = TKT_TILE_ITEM;
			}
	}

	// connect all walls into one mass, then guarantee every open space is
	// reachable with minimal carving; repeated because the wall bridges can
	// block corridors and the open repair can split thin walls
	ConnectWalls(Map);
	ConnectOpenSpaces(Map, Seed ^ 0xA5A5A5A5ULL);
	ConnectWalls(Map);
	ConnectOpenSpaces(Map, Seed ^ 0x5A5A5A5AULL);

	return Map;
}

// TEMP: count open regions of the finished map
static int TKTCountRegions(const STKTMapData &Map)
{
	std::vector<unsigned char> V(Map.m_Width * Map.m_Height, 0);
	int R = 0;
	for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
	{
		if(Map.m_aTiles[i] == TKT_TILE_WALL || V[i])
			continue;
		R++;
		std::vector<int> Q;
		Q.push_back(i);
		V[i] = 1;
		for(size_t H = 0; H < Q.size(); ++H)
		{
			int Pos = Q[H];
			int x = Pos % Map.m_Width;
			int y = Pos / Map.m_Width;
			static const int dx[4] = {0, 0, -1, 1};
			static const int dy[4] = {-1, 1, 0, 0};
			for(int d = 0; d < 4; ++d)
			{
				int nx = x + dx[d], ny = y + dy[d];
				if(!Map.InBounds(nx, ny) || Map.At(nx, ny) == TKT_TILE_WALL)
					continue;
				int NPos = ny * Map.m_Width + nx;
				if(!V[NPos])
				{
					V[NPos] = 1;
					Q.push_back(NPos);
				}
			}
		}
	}
	return R;
}

// ddnet_walls automap rules, taken from ddnet's data/editor/automap/ddnet_walls.rules
static const char *const s_pDdnetWallsRules =
"[Basic Walls]\nIndex 16\n\n#1W2W3W4W\nIndex 17\nPos 0 -1 EMPTY\nPos 0 1 EMPTY\nPos -1 0 EMPTY\nPos 1 0 EMPTY\n\n#1W2W3S4W\nIndex 18\nPos 0 -1 EMPTY\nPos -1 0 EMPTY\nPos 0 1 FULL\nPos 1 0 EMPTY\n\n#1W2W3S4W\nIndex 18 YFLIP\nPos 0 -1 FULL\nPos -1 0 EMPTY\nPos 0 1 EMPTY\nPos 1 0 EMPTY\n\n#1W2S3S4W\nIndex 19\nPos 0 -1 EMPTY\nPos -1 0 EMPTY\nPos 0 1 FULL\nPos 1 0 FULL\nPos 1 1 FULL\n\n#1W2S3S4W\nIndex 19 YFLIP\nPos 0 -1 FULL\nPos -1 0 EMPTY\nPos 0 1 EMPTY\nPos 1 0 FULL\nPos 1 -1 FULL\n\n#1W2S3S4W\nIndex 19 XFLIP\nPos 0 -1 EMPTY\nPos 0 1 FULL\nPos -1 0 FULL\nPos 1 0 EMPTY\nPos -1 1 FULL\n\n#1W2S3S4W\nIndex 19 XFLIP YFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos -1 0 FULL\nPos 1 0 EMPTY\nPos 0 1 EMPTY\n\n#1W2S3W4S\nIndex 20\nPos -1 0 FULL\nPos 1 0 FULL\nPos 0 -1 EMPTY\nPos 0 1 EMPTY\n\n#1W2S3S4S\nIndex 21\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 FULL\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 -1 EMPTY\n\n#1W2S3S4S\nIndex 21 YFLIP\nPos -1 -1 FULL\nPos 1 -1 FULL\nPos 0 -1 FULL\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 1 EMPTY\n\n#1D2D3D4D\nIndex 32\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1D2S3D4D\nIndex 33\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1D2S3D4D\nIndex 33 XFLIP\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1D2S3D4D\nIndex 33 YFLIP\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1D2S3D4D\nIndex 33 XFLIP YFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1D2S3S4D\nIndex 34\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1D2S3S4D\nIndex 34 YFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1S2S3S4D\nIndex 35\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1S2S3S4D\nIndex 35 XFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1S2S3S4D\nIndex 35 YFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1S2S3S4D\nIndex 35 XFLIP YFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1S2D3S4D\nIndex 36\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1S2D3S4D\nIndex 36 XFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1S2S3D4D\nIndex 37\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 -1 FULL\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 FULL\n\n#1S2S3D4D\nIndex 37 XFLIP\nPos -1 -1 FULL\nPos 0 -1 FULL\nPos 1 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 FULL\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 EMPTY\n\n#1W2D3S4W\nIndex 48\nPos 0 -1 EMPTY\nPos -1 0 EMPTY\nPos 0 1 FULL\nPos 1 1 EMPTY\nPos 1 0 FULL\n\n#1W2D3S4W\nIndex 48 XFLIP\nPos 0 -1 EMPTY\nPos 1 0 EMPTY\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos -1 0 FULL\n\n#1W2D3S4W\nIndex 48 YFLIP\nPos -1 0 EMPTY\nPos 0 1 EMPTY\nPos 1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 0 FULL\n\n#1W2D3S4W\nIndex 48 XFLIP YFLIP\nPos 1 0 EMPTY\nPos 0 1 EMPTY\nPos -1 -1 EMPTY\nPos 0 -1 FULL\nPos -1 0 FULL\n\n#1W2D3D4S\nIndex 49\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 EMPTY\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 -1 EMPTY\n\n#1W2D3D4S\nIndex 49 YFLIP\nPos -1 -1 EMPTY\nPos 1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 1 EMPTY\n\n#1W2S3D4S\nIndex 50\nPos -1 1 EMPTY\nPos 0 1 FULL\nPos 1 1 FULL\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 -1 EMPTY\n\n#1W2S3D4S\nIndex 50 XFLIP\nPos -1 1 FULL\nPos 0 1 FULL\nPos 1 1 EMPTY\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 -1 EMPTY\n\n#1W2S3D4S\nIndex 50 YFLIP\nPos -1 -1 EMPTY\nPos 1 -1 FULL\nPos 0 -1 FULL\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 1 EMPTY\n\n#1W2S3D4S\nIndex 50 XFLIP YFLIP\nPos -1 -1 FULL\nPos 1 -1 EMPTY\nPos 0 -1 FULL\nPos 1 0 FULL\nPos -1 0 FULL\nPos 0 1 EMPTY\n\n#1S2W3S4W\nIndex 51\nPos -1 0 EMPTY\nPos 1 0 EMPTY\nPos 0 -1 FULL\nPos 0 1 FULL\n\n#1W2S3W4W\nIndex 52\nPos 0 -1 EMPTY\nPos -1 0 EMPTY\nPos 1 0 FULL\nPos 0 1 EMPTY\n\n#1W2S3W4W\nIndex 52 XFLIP\nPos 0 -1 EMPTY\nPos -1 0 FULL\nPos 1 0 EMPTY\nPos 0 1 EMPTY\n\n#1S2S3S4W\nIndex 53\nPos 0 -1 FULL\nPos -1 0 EMPTY\nPos 1 0 FULL\nPos 0 1 FULL\nPos 1 -1 FULL\nPos 1 1 FULL\n\n#1S2S3S4W\nIndex 53 XFLIP\nPos 0 -1 FULL\nPos -1 -1 FULL\nPos -1 0 FULL\nPos -1 1 FULL\nPos 1 0 EMPTY\nPos 0 1 FULL\n\n#1D2D3S4W\nIndex 64\nPos 0 -1 FULL\nPos -1 0 EMPTY\nPos 1 0 FULL\nPos 0 1 FULL\nPos 1 -1 EMPTY\nPos 1 1 EMPTY\n\n#1D2D3S4W\nIndex 64 XFLIP\nPos 0 -1 FULL\nPos -1 -1 EMPTY\nPos -1 0 FULL\nPos -1 1 EMPTY\nPos 1 0 EMPTY\nPos 0 1 FULL\n\n#1S2W3D4S\nIndex 65\nPos 0 -1 FULL\nPos -1 -1 FULL\nPos -1 0 FULL\nPos -1 1 EMPTY\nPos 1 0 EMPTY\nPos 0 1 FULL\n\n#1S2W3D4S\nIndex 65 XFLIP\nPos 0 -1 FULL\nPos -1 0 EMPTY\nPos 1 0 FULL\nPos 0 1 FULL\nPos 1 -1 FULL\nPos 1 1 EMPTY\n\n#1S2W3D4S\nIndex 65 YFLIP\nPos 0 -1 FULL\nPos -1 -1 EMPTY\nPos -1 0 FULL\nPos -1 1 FULL\nPos 1 0 EMPTY\nPos 0 1 FULL\n\n#1S2W3D4S\nIndex 65 XFLIP YFLIP\nPos 0 -1 FULL\nPos -1 0 EMPTY\nPos 1 0 FULL\nPos 0 1 FULL\nPos 1 -1 EMPTY\nPos 1 1 FULL\n";

// ddnet automap: index rule with position constraints
struct STKTAutomapRule
{
	int m_Id;
	int m_Flag;
	struct SPos
	{
		int m_X;
		int m_Y;
		bool m_Full; // false = the neighbour must be empty
	};
	std::vector<SPos> m_vPos;
};

static const std::vector<STKTAutomapRule> &GetWallAutomapRules()
{
	static std::vector<STKTAutomapRule> s_vRules;
	if(s_vRules.empty())
	{
		// parse the embedded rules
		std::vector<std::string> Lines;
		std::string Current;
		for(const char *p = s_pDdnetWallsRules; *p; ++p)
		{
			if(*p == '\n')
			{
				Lines.push_back(Current);
				Current.clear();
			}
			else
				Current += *p;
		}
		if(!Current.empty())
			Lines.push_back(Current);

		for(const std::string &Line : Lines)
		{
			if(Line.empty() || Line[0] == '#' || Line[0] == '[')
				continue;
			if(Line.compare(0, 5, "Index") == 0)
			{
				STKTAutomapRule Rule;
				int Id = 0;
				char aFlags[4][64] = {{0}, {0}, {0}, {0}};
				sscanf(Line.c_str(), "Index %d %63s %63s %63s %63s", &Id, aFlags[0], aFlags[1], aFlags[2], aFlags[3]);
				Rule.m_Id = Id;
				for(int i = 0; i < 4; ++i)
				{
					if(str_comp(aFlags[i], "XFLIP") == 0)
						Rule.m_Flag |= 1;
					else if(str_comp(aFlags[i], "YFLIP") == 0)
						Rule.m_Flag |= 2;
					else if(str_comp(aFlags[i], "ROTATE") == 0)
						Rule.m_Flag |= 8;
				}
				s_vRules.push_back(Rule);
			}
			else if(Line.compare(0, 3, "Pos") == 0 && !s_vRules.empty())
			{
				STKTAutomapRule::SPos Pos;
				char aValue[64];
				sscanf(Line.c_str(), "Pos %d %d %63s", &Pos.m_X, &Pos.m_Y, aValue);
				Pos.m_Full = str_comp(aValue, "FULL") == 0;
				s_vRules.back().m_vPos.push_back(Pos);
			}
		}
	}
	return s_vRules;
}

// apply the ddnet_walls automap to a wall tile, returns the tile index and flags
static int AutomapWall(const STKTMapData &Map, int x, int y, int &OutFlags)
{
	auto Wall = [&](int X, int Y) {
		return !Map.InBounds(X, Y) || Map.At(X, Y) == TKT_TILE_WALL;
	};

	const std::vector<STKTAutomapRule> &vRules = GetWallAutomapRules();
	for(const STKTAutomapRule &Rule : vRules)
	{
		bool Matches = true;
		for(const STKTAutomapRule::SPos &Pos : Rule.m_vPos)
		{
			bool IsWall = Wall(x + Pos.m_X, y + Pos.m_Y);
			if(Pos.m_Full ? !IsWall : IsWall)
			{
				Matches = false;
				break;
			}
		}
		if(Matches)
		{
			OutFlags = Rule.m_Flag;
			return Rule.m_Id;
		}
	}
	OutFlags = 0;
	return 0;
}

// shared layer writer: game layer, ddnet_walls layer and an optional speedup
// layer carrying the mega map index tiles
static void WriteTKTMapLayers(IStorage *pStorage, const char *pPath, const STKTMapData &Map,
	const std::vector<CSpeedupTile> *pSpeedupTiles)
{
	CDataFileWriter Writer;
	if(!Writer.Open(pStorage, pPath))
	{
		dbg_msg("tkt_mapgen", "couldn't open map file '%s' for writing", pPath);
		return;
	}

	// map version item
	int Version = 1;
	Writer.AddItem(MAPITEMTYPE_VERSION, 0, sizeof(Version), &Version);

	// the ddnet_walls image (external, the client resolves it by name)
	CMapItemImage Image;
	Image.m_Version = 1;
	Image.m_Width = 1024;
	Image.m_Height = 1024;
	Image.m_External = 1;
	Image.m_ImageName = Writer.AddData(str_length("ddnet_walls") + 1, "ddnet_walls");
	Image.m_ImageData = -1;
	Writer.AddItem(MAPITEMTYPE_IMAGE, 0, sizeof(Image), &Image);

	// one group containing all layers
	CMapItemGroup Group;
	Group.m_Version = 3;
	Group.m_OffsetX = 0;
	Group.m_OffsetY = 0;
	Group.m_ParallaxX = 100;
	Group.m_ParallaxY = 100;
	Group.m_StartLayer = 0;
	Group.m_NumLayers = pSpeedupTiles ? 3 : 2;
	Group.m_UseClipping = 0;
	Group.m_ClipX = 0;
	Group.m_ClipY = 0;
	Group.m_ClipW = 0;
	Group.m_ClipH = 0;
	Group.m_aName[0] = 0;
	Group.m_aName[1] = 0;
	Group.m_aName[2] = 0;
	Writer.AddItem(MAPITEMTYPE_GROUP, 0, sizeof(Group), &Group);

	// build the tile arrays
	std::vector<CTile> GameTiles(Map.m_Width * Map.m_Height);
	std::vector<CTile> WallTiles(Map.m_Width * Map.m_Height);
	for(int y = 0; y < Map.m_Height; ++y)
	{
		for(int x = 0; x < Map.m_Width; ++x)
		{
			int Tile = Map.At(x, y);
			CTile &GameTile = GameTiles[y * Map.m_Width + x];
			GameTile.m_Index = Tile == TKT_TILE_WALL ? 1 : (Tile == TKT_TILE_SPAWN ? 192 : (Tile == TKT_TILE_ITEM ? 189 : 0));
			GameTile.m_Flags = 0;
			GameTile.m_Skip = 0;
			GameTile.m_Reserved = 0;

			CTile &WallTile = WallTiles[y * Map.m_Width + x];
			if(Tile == TKT_TILE_WALL)
			{
				int Flags = 0;
				WallTile.m_Index = AutomapWall(Map, x, y, Flags);
				WallTile.m_Flags = Flags;
			}
			else
			{
				WallTile.m_Index = 0;
				WallTile.m_Flags = 0;
			}
			WallTile.m_Skip = 0;
			WallTile.m_Reserved = 0;
		}
	}

	// speedup tile data, referenced only by the speedup layer; the game
	// layer keeps m_Speedup = -1 so clients do not read the mega map index
	// tiles as speedup zones; the speedup layer itself also gets an empty
	// CTile array as m_Data, matching the official mega maps, so clients
	// that render the layer always have valid data to read
	int SpeedupData = -1;
	int SpeedupLayerData = -1;
	if(pSpeedupTiles)
	{
		SpeedupData = Writer.AddData(pSpeedupTiles->size() * sizeof(CSpeedupTile), (void *)&(*pSpeedupTiles)[0]);
		std::vector<CTile> EmptyTiles(Map.m_Width * Map.m_Height);
		SpeedupLayerData = Writer.AddData(EmptyTiles.size() * sizeof(CTile), &EmptyTiles[0]);
	}

	// game layer
	CMapItemLayerTilemap GameLayer;
	GameLayer.m_Layer.m_Version = 0;
	GameLayer.m_Layer.m_Type = LAYERTYPE_TILES;
	GameLayer.m_Layer.m_Flags = 0;
	GameLayer.m_Version = 2;
	GameLayer.m_Width = Map.m_Width;
	GameLayer.m_Height = Map.m_Height;
	GameLayer.m_Flags = TILESLAYERFLAG_GAME;
	GameLayer.m_Color.r = 0;
	GameLayer.m_Color.g = 0;
	GameLayer.m_Color.b = 0;
	GameLayer.m_Color.a = 0;
	GameLayer.m_ColorEnv = -1;
	GameLayer.m_ColorEnvOffset = 0;
	GameLayer.m_Image = -1;
	GameLayer.m_Data = Writer.AddData(GameTiles.size() * sizeof(CTile), &GameTiles[0]);
	GameLayer.m_aName[0] = 0;
	GameLayer.m_aName[1] = 0;
	GameLayer.m_aName[2] = 0;
	GameLayer.m_Tele = -1;
	GameLayer.m_Speedup = -1; // never expose speedup data through the game layer
	GameLayer.m_Front = -1;
	GameLayer.m_Switch = -1;
	GameLayer.m_Tune = -1;
	Writer.AddItem(MAPITEMTYPE_LAYER, 0, sizeof(GameLayer), &GameLayer);

	// ddnet_walls layer with the automap textures
	CMapItemLayerTilemap WallsLayer;
	WallsLayer.m_Layer.m_Version = 0;
	WallsLayer.m_Layer.m_Type = LAYERTYPE_TILES;
	WallsLayer.m_Layer.m_Flags = 0;
	WallsLayer.m_Version = 2;
	WallsLayer.m_Width = Map.m_Width;
	WallsLayer.m_Height = Map.m_Height;
	WallsLayer.m_Flags = 0;
	WallsLayer.m_Color.r = 255;
	WallsLayer.m_Color.g = 255;
	WallsLayer.m_Color.b = 255;
	WallsLayer.m_Color.a = 255;
	WallsLayer.m_ColorEnv = -1;
	WallsLayer.m_ColorEnvOffset = 0;
	WallsLayer.m_Image = 0;
	WallsLayer.m_Data = Writer.AddData(WallTiles.size() * sizeof(CTile), &WallTiles[0]);
	WallsLayer.m_aName[0] = 0;
	WallsLayer.m_aName[1] = 0;
	WallsLayer.m_aName[2] = 0;
	WallsLayer.m_Tele = -1;
	WallsLayer.m_Speedup = -1;
	WallsLayer.m_Front = -1;
	WallsLayer.m_Switch = -1;
	WallsLayer.m_Tune = -1;
	Writer.AddItem(MAPITEMTYPE_LAYER, 1, sizeof(WallsLayer), &WallsLayer);

	// speedup layer carrying the mega map indices
	if(pSpeedupTiles)
	{
		CMapItemLayerTilemap SpeedupLayer;
		SpeedupLayer.m_Layer.m_Version = 0;
		SpeedupLayer.m_Layer.m_Type = LAYERTYPE_TILES;
		SpeedupLayer.m_Layer.m_Flags = 0;
		SpeedupLayer.m_Version = 3; // version 3 keeps m_Speedup at its struct position
		SpeedupLayer.m_Width = Map.m_Width;
		SpeedupLayer.m_Height = Map.m_Height;
		SpeedupLayer.m_Flags = TILESLAYERFLAG_SPEEDUP;
		SpeedupLayer.m_Color.r = 0;
		SpeedupLayer.m_Color.g = 0;
		SpeedupLayer.m_Color.b = 0;
		SpeedupLayer.m_Color.a = 0;
		SpeedupLayer.m_ColorEnv = -1;
		SpeedupLayer.m_ColorEnvOffset = 0;
		SpeedupLayer.m_Image = 0; // the walls image, clients rendering this layer see empty tiles
		SpeedupLayer.m_Data = SpeedupLayerData;
		SpeedupLayer.m_aName[0] = 0;
		SpeedupLayer.m_aName[1] = 0;
		SpeedupLayer.m_aName[2] = 0;
		SpeedupLayer.m_Tele = -1;
		SpeedupLayer.m_Speedup = SpeedupData;
		SpeedupLayer.m_Front = -1;
		SpeedupLayer.m_Switch = -1;
		SpeedupLayer.m_Tune = -1;
		Writer.AddItem(MAPITEMTYPE_LAYER, 2, sizeof(SpeedupLayer), &SpeedupLayer);
	}

	Writer.Finish();

	dbg_msg("tkt_mapgen", "wrote '%s' (%dx%d, %d walls, %d spawns, %d items)",
		pPath, Map.m_Width, Map.m_Height,
		(int)std::count(Map.m_aTiles.begin(), Map.m_aTiles.end(), TKT_TILE_WALL),
		(int)std::count(Map.m_aTiles.begin(), Map.m_aTiles.end(), TKT_TILE_SPAWN),
		(int)std::count(Map.m_aTiles.begin(), Map.m_aTiles.end(), TKT_TILE_ITEM));
}

// generate a map from a seed and write it to the given storage path,
// returns the generated map data
static STKTMapData WriteTKTMapFile(IStorage *pStorage, uint64_t Seed, int Width, int Height, const char *pPath)
{
	STKTMapData Map = GenerateTKTMap(Seed, Width, Height);
	WriteTKTMapLayers(pStorage, pPath, Map, 0);
	return Map;
}

// generate NumArenas random arenas packed side by side into one mega map
// (each arena is walled off from its neighbours and marked with its index
// in the speedup layer), write the map and the mega_add_mapname config next
// to it; the engine executes the config on load and the arena entities are
// filtered per submap by the controller
static STKTMapData WriteMegaTKTMapFile(IStorage *pStorage, uint64_t Seed, int NumArenas, int ArenaWidth, int ArenaHeight, const char *pPath)
{
	int Cols = (int)std::ceil(std::sqrt((double)NumArenas));
	int Rows = (NumArenas + Cols - 1) / Cols;
	int BlockW = ArenaWidth + 2;
	int BlockH = ArenaHeight + 2;
	int Width = Cols * BlockW + 2;
	int Height = Rows * BlockH + 2;

	STKTMapData Map;
	Map.m_Width = Width;
	Map.m_Height = Height;
	Map.m_aTiles.assign(Width * Height, TKT_TILE_WALL);

	CTKTRng Rng(Seed);
	std::vector<int> aArenaX0(NumArenas), aArenaY0(NumArenas);
	for(int a = 0; a < NumArenas; ++a)
	{
		int Col = a % Cols;
		int Row = a / Cols;
		int X0 = 2 + Col * BlockW;
		int Y0 = 2 + Row * BlockH;
		uint64_t ArenaSeed = ((uint64_t)Rng.Next() << 32) | Rng.Next();
		STKTMapData Arena = GenerateTKTMap(ArenaSeed, ArenaWidth, ArenaHeight);
		for(int y = 0; y < ArenaHeight; ++y)
			for(int x = 0; x < ArenaWidth; ++x)
				Map.At(X0 + x, Y0 + y) = Arena.At(x, y);
		aArenaX0[a] = X0;
		aArenaY0[a] = Y0;
	}

	// speedup layer: mark each arena with its mega map index
	std::vector<CSpeedupTile> SpeedupTiles(Width * Height);
	for(int a = 0; a < NumArenas; ++a)
	{
		for(int y = 0; y < ArenaHeight; ++y)
		{
			for(int x = 0; x < ArenaWidth; ++x)
			{
				CSpeedupTile &S = SpeedupTiles[(aArenaY0[a] + y) * Width + (aArenaX0[a] + x)];
				S.m_Force = 0;
				S.m_MaxSpeed = a + 1;
				S.m_Type = TILE_MEGAMAP_INDEX;
				S.m_Angle = 0;
			}
		}
	}

	// submap name config, auto-executed by the engine on map load
	{
		char aCfgPath[256];
		str_format(aCfgPath, sizeof(aCfgPath), "%s.cfg", pPath);
		IOHANDLE Cfg = pStorage->OpenFile(aCfgPath, IOFLAG_WRITE, IStorage::TYPE_SAVE);
		if(Cfg)
		{
			for(int a = 0; a < NumArenas; ++a)
			{
				char aLine[64];
				int Len = str_format(aLine, sizeof(aLine), "mega_add_mapname tkt_%d\n", a + 1);
				io_write(Cfg, aLine, Len);
			}
			io_close(Cfg);
		}
	}

	WriteTKTMapLayers(pStorage, pPath, Map, &SpeedupTiles);
	return Map;
}

// helper: generate with a random size in 50..200 (for the standalone tools)
static STKTMapData GenerateTKTMap(uint64_t Seed)
{
	CTKTRng Rng(Seed);
	int Width = 50 + Rng.Below(151);
	int Height = 50 + Rng.Below(151);
	return GenerateTKTMap(Seed, Width, Height);
}

// helper: generate with a random size in 50..200 (for the standalone tool)
static STKTMapData WriteTKTMapFile(IStorage *pStorage, uint64_t Seed, const char *pPath)
{
	CTKTRng Rng(Seed);
	int Width = 50 + Rng.Below(151);
	int Height = 50 + Rng.Below(151);
	return WriteTKTMapFile(pStorage, Seed, Width, Height, pPath);
}

#endif // TKT_MAPGEN_H