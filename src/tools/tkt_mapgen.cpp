#include <base/system.h>
#include <engine/storage.h>

#include "tkt_mapgen.h"

#include <stdio.h>

int main(int argc, const char **argv)
{
	dbg_logger_stdout();
	IStorage *pStorage = CreateStorage("Teeworlds", IStorage::STORAGETYPE_SERVER, argc, argv);

	if(argc < 2)
	{
		printf("usage: tkt_mapgen <seed> [output]\n");
		return 1;
	}

	uint64_t Seed = strtoull(argv[1], NULL, 0);
	char aPath[128];
	if(argc >= 3)
		str_copy(aPath, argv[2], sizeof(aPath));
	else
		str_format(aPath, sizeof(aPath), "maps/tkt_gen_%llu.map", (unsigned long long)Seed);

	STKTMapData Map = WriteTKTMapFile(pStorage, Seed, aPath);
	printf("wrote '%s' (%dx%d, %d walls, %d spawns, %d items)\n",
		aPath, Map.m_Width, Map.m_Height,
		(int)std::count(Map.m_aTiles.begin(), Map.m_aTiles.end(), TKT_TILE_WALL),
		(int)std::count(Map.m_aTiles.begin(), Map.m_aTiles.end(), TKT_TILE_SPAWN),
		(int)std::count(Map.m_aTiles.begin(), Map.m_aTiles.end(), TKT_TILE_ITEM));
	return 0;
}
