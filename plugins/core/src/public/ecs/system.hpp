#pragma once

struct World;
struct ExecContext;

struct SystemBase {
	virtual ~SystemBase() = default;
	virtual auto execute(ExecContext& context) -> void = 0;
};