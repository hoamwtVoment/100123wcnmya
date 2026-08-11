#include <base/system.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>
#include <game/mapitems.h>

#include "tkt_mapgen.h"

#include <stdio.h>

// render a generated map grid from a seed
static void RenderGenerated(uint64_t Seed)
{
	STKTMapData Map = GenerateTKTMap(Seed);

	// count the wall regions too (1 = all walls connected together)
	int WallRegions = 0;
	{
		std::vector<unsigned char> V(Map.m_Width * Map.m_Height, 0);
		for(int i = 0; i < Map.m_Width * Map.m_Height; ++i)
		{
			if(Map.m_aTiles[i] != TKT_TILE_WALL || V[i])
				continue;
			WallRegions++;
			std::vector<int> Q;
			Q.push_back(i);
			V[i] = 1;
			for(size_t H = 0; H < Q.size(); ++H)
			{
				int Pos = Q[H];
				int x = Pos % Map.m_Width;
				int y = Pos / Map.m_Width;
				static const int s_Dx[4] = {0, 0, -1, 1};
				static const int s_Dy[4] = {-1, 1, 0, 0};
				for(int d = 0; d < 4; ++d)
				{
					int nx = x + s_Dx[d];
					int ny = y + s_Dy[d];
					if(!Map.InBounds(nx, ny) || Map.At(nx, ny) != TKT_TILE_WALL)
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
	}
	printf("seed=%llu size=%dx%d open_regions=%d wall_regions=%d\n", (unsigned long long)Seed, Map.m_Width, Map.m_Height, TKTCountRegions(Map), WallRegions);
	for(int y = 0; y < Map.m_Height; ++y)
	{
		for(int x = 0; x < Map.m_Width; ++x)
		{
			switch(Map.At(x, y))
			{
			case TKT_TILE_WALL:
				putchar('#');
				break;
			case TKT_TILE_SPAWN:
				putchar('S');
				break;
			case TKT_TILE_ITEM:
				putchar('I');
				break;
			default:
				putchar('.');
				break;
			}
		}
		putchar('\n');
	}
}

// parse and render an existing map file (also useful to inspect templates)
static void RenderMapFile(IStorage *pStorage, const char *pPath)
{
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, pPath, IStorage::TYPE_ALL))
	{
		printf("couldn't open map file '%s'\n", pPath);
		return;
	}

	int GroupsStart, GroupsNum, LayersStart, LayersNum, ImagesStart, ImagesNum;
	Reader.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &GroupsNum);
	Reader.GetType(MAPITEMTYPE_LAYER, &LayersStart, &LayersNum);
	Reader.GetType(MAPITEMTYPE_IMAGE, &ImagesStart, &ImagesNum);

	for(int i = ImagesStart; i < ImagesStart + ImagesNum; ++i)
	{
		int Type, ID;
		CMapItemImage *pImage = (CMapItemImage *)Reader.GetItem(i, &Type, &ID);
		if(pImage->m_Version == 1)
		{
			const char *pName = (const char *)Reader.GetData(pImage->m_ImageName);
			printf("image: %dx%d external=%d name='%s'\n", pImage->m_Width, pImage->m_Height, pImage->m_External, pName ? pName : "?");
		}
	}

	for(int i = LayersStart; i < LayersStart + LayersNum; ++i)
	{
		int Type, ID;
		void *pRawItem = Reader.GetItem(i, &Type, &ID);
		int ItemSize = Reader.GetItemSize(i);
		CMapItemLayer *pLayer = (CMapItemLayer *)pRawItem;
		if(pLayer->m_Type != LAYERTYPE_TILES)
			continue;

		bool IsExtended = ItemSize >= (int)sizeof(CMapItemLayerTilemap);
		CMapItemLayerTilemap *pTilemap = (CMapItemLayerTilemap *)pRawItem;
		const char *pName = IsExtended ? (const char *)pTilemap->m_aName : "";
		printf("tilemap: %dx%d flags=%d image=%d data=%d name='%s' extended=%d\n",
			pTilemap->m_Width, pTilemap->m_Height, pTilemap->m_Flags, pTilemap->m_Image, pTilemap->m_Data,
			IsExtended ? pName : "?", IsExtended);

		if(pTilemap->m_Data < 0)
			continue;
		CTile *pTiles = (CTile *)Reader.GetData(pTilemap->m_Data);
		int W = pTilemap->m_Width;
		int H = pTilemap->m_Height;

		// count distinct tile indices
		int Counts[1024] = {0};
		int NonZero = 0;
		for(int y = 0; y < H; ++y)
			for(int x = 0; x < W; ++x)
			{
				int Index = pTiles[y * W + x].m_Index;
				if(Index)
					NonZero++;
				if(Index >= 0 && Index < 1024)
					Counts[Index]++;
			}
		printf("  nonzero tiles: %d\n", NonZero);
		for(int t = 0; t < 1024; ++t)
			if(Counts[t])
				printf("  index %d: %d\n", t, Counts[t]);

		// analyze the automap: neighbor wall pattern -> walls layer index
		if(pTilemap->m_Image >= 0)
		{
			// find the game layer to compare neighbors
			for(int j = LayersStart; j < LayersStart + LayersNum; ++j)
			{
				int JType, JID;
				CMapItemLayer *pLayer2 = (CMapItemLayer *)Reader.GetItem(j, &JType, &JID);
				if(pLayer2->m_Type != LAYERTYPE_TILES)
					continue;
				CMapItemLayerTilemap *pGameLayer = (CMapItemLayerTilemap *)pLayer2;
				if(!(pGameLayer->m_Flags & TILESLAYERFLAG_GAME) || pGameLayer->m_Data < 0)
					continue;
				CTile *pGameTiles = (CTile *)Reader.GetData(pGameLayer->m_Data);
				int GW = pGameLayer->m_Width;
				int GH = pGameLayer->m_Height;

				printf("  automap: neighbor mask (up|down|left|right walls) -> index\n");
				for(int y = 1; y < GH - 1; ++y)
					for(int x = 1; x < GW - 1; ++x)
					{
						if(pGameTiles[y * GW + x].m_Index != 1)
							continue;
						int Mask = 0;
						if(pGameTiles[(y - 1) * GW + x].m_Index == 1)
							Mask |= 1;
						if(pGameTiles[(y + 1) * GW + x].m_Index == 1)
							Mask |= 2;
						if(pGameTiles[y * GW + x - 1].m_Index == 1)
							Mask |= 4;
						if(pGameTiles[y * GW + x + 1].m_Index == 1)
							Mask |= 8;
						int WallIndex = pTiles[y * W + x].m_Index;
						printf("    mask=%d (%c%c%c%c) -> index %d\n", Mask,
							(Mask & 1) ? 'U' : ' ', (Mask & 2) ? 'D' : ' ', (Mask & 4) ? 'L' : ' ', (Mask & 8) ? 'R' : ' ',
							WallIndex);
					}
				break;
			}
		}

		// render, capped for readability
		bool IsWallsLayer = pTilemap->m_Image >= 0;
		int RenderH = H > 40 ? 40 : H;
		for(int y = 0; y < RenderH; ++y)
		{
			for(int x = 0; x < W; ++x)
			{
				int Index = pTiles[y * W + x].m_Index;
				char C;
				if(IsWallsLayer)
				{
					// map the automap indices to letters
					if(Index == 0)
						C = '.';
					else if(Index >= 16 && Index < 16 + 26)
						C = 'A' + (Index - 16);
					else if(Index >= 32 && Index < 32 + 26)
						C = 'a' + (Index - 32);
					else
						C = '?';
				}
				else
				{
					if(Index == 1)
						C = '#';
					else if(Index == 192)
						C = 'S';
					else if(Index == 189)
						C = 'I';
					else if(Index == 0)
						C = '.';
					else
						C = '?';
				}
				putchar(C);
			}
			putchar('\n');
		}
	}
}

int main(int argc, const char **argv)
{
	dbg_logger_stdout();
	IStorage *pStorage = CreateStorage("Teeworlds", IStorage::STORAGETYPE_SERVER, argc, argv);

	if(argc >= 3 && str_comp(argv[1], "-load") == 0)
	{
		RenderMapFile(pStorage, argv[2]);
		return 0;
	}

	if(argc < 2)
	{
		printf("usage:\n");
		printf("  tkt_mapview <seed>            render the generated map grid\n");
		printf("  tkt_mapview -load <file.map>  parse and render a map file\n");
		return 1;
	}

	uint64_t Seed = strtoull(argv[1], NULL, 0);
	RenderGenerated(Seed);
	return 0;
}
