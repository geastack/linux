/* targets/raspberry-pi-os/include/rpios_prelude.h
 * Forced-include header (-include) for the raspberry-pi-os build to pull in standard
 * headers that geatsc's runtime emit depends on but doesn't explicitly
 * include (same role as targets/geaos/include/geaos_prelude.h; gcc libstdc++
 * requires them up front). */
#pragma once
#include <optional>
#include <variant>
#include <functional>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <set>
