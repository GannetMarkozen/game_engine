#pragma once

#include "types.hpp"

struct ComponentId : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};

struct ArchetypeId : public IntAlias<u16> {
	using IntAlias<u16>::IntAlias;
};