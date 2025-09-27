#!/usr/bin/env bash
set -e
BASE="${1:-http://ledcontroller.local}"
curl "$BASE/on"
curl "$BASE/api/color?hex=00FF80"
curl "$BASE/api/brightness?value=200"
curl "$BASE/api/state"
