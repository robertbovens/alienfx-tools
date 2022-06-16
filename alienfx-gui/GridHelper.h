#pragma once
#include "alienfx-gui.h"
#include "common.h"
#include <random>

class GridHelper {
	GridHelper();
	~GridHelper();
public:
	UINT tact = 0;
	mt19937 rnd;
	ThreadHelper* gridTrigger = NULL, *gridThread = NULL;
};