#!/usr/bin/env bash
# Protocol-level test for the resident dispatch daemon.
#
# src/test.c exercises the engine by calling it directly. This exercises it the
# way the web bridge actually does: over a TCP socket, one plain-text command
# per line in, one JSON object per line out. It checks the wire contract --
# reply shape, ordering under pipelining, error handling for malformed input,
# and that state-changing commands really change state.
#
# No dependencies beyond bash: the client is bash's own /dev/tcp.
#
#   ./scripts/protocol_test.sh [port]      exits non-zero if any check fails

set -uo pipefail
cd "$(dirname "$0")/.."

PORT="${1:-19099}"
CHECKS=0
FAILS=0
GREEN=$'\033[32m'; RED=$'\033[31m'; BOLD=$'\033[1m'; OFF=$'\033[0m'

ok() {   # ok <condition-result 0/1> <name> [detail]
    CHECKS=$((CHECKS + 1))
    if [ "$1" = "1" ]; then
        printf '  %sPASS%s  %s\n' "$GREEN" "$OFF" "$2"
    else
        FAILS=$((FAILS + 1))
        printf '  %sFAIL%s  %s\n' "$RED" "$OFF" "$2"
        [ -n "${3:-}" ] && printf '        %s\n' "$3"
    fi
}
match() { # match <reply> <substring> <name>
    case "$1" in *"$2"*) ok 1 "$3" ;; *) ok 0 "$3" "got: $1" ;; esac
}
# pull a numeric JSON field out of a reply line
field() { printf '%s' "$1" | sed -n "s/.*\"$2\":\([-0-9.]*\).*/\1/p"; }

printf '%sHealthWay — daemon protocol suite%s\n' "$BOLD" "$OFF"

make -s server || { echo "build failed"; exit 1; }

./server "$PORT" >/dev/null 2>/tmp/healthway-proto.log &
SRV=$!
trap 'kill $SRV 2>/dev/null; wait $SRV 2>/dev/null' EXIT

# The engine builds the graph and the distance table before it binds. Poll
# rather than sleeping a fixed amount, so a slow machine does not fail here.
for _ in $(seq 1 200); do
    (exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null && break
    sleep 0.1
done
(exec 3<>/dev/tcp/127.0.0.1/"$PORT") 2>/dev/null \
    || { echo "engine never came up; see /tmp/healthway-proto.log"; exit 1; }

# --- one connection, everything pipelined: send all commands, read all replies
send() {   # send <cmd...>  -> replies, one per line, in REPLY[]
    local n=$#
    exec 3<>/dev/tcp/127.0.0.1/"$PORT"
    printf '%s\n' "$@" >&3
    REPLY=()
    local i line
    for ((i = 0; i < n; i++)); do
        IFS= read -r -t 20 line <&3 || break
        REPLY+=("$line")
    done
    printf 'QUIT\n' >&3
    exec 3<&-
}

printf '\n%s1. handshake and static map data%s\n' "$BOLD" "$OFF"
send "STATS" "BOUNDS" "HOSPITALS" "FLEET" "NODE 0"
ok $([ ${#REPLY[@]} -eq 5 ] && echo 1 || echo 0) \
   "five pipelined commands return five replies, in order" "got ${#REPLY[@]}"
match "${REPLY[0]}" '"ok":true'      "STATS reports engine state"
match "${REPLY[1]}" '"minx"'         "BOUNDS returns the map extent"
match "${REPLY[2]}" '"ok":true'      "HOSPITALS returns the hospital list"
match "${REPLY[3]}" '"ok":true'      "FLEET returns the ambulance list"
match "${REPLY[4]}" '"node"'         "NODE resolves a village to a road node"

NODES=$(field "${REPLY[0]}" nodes)
DISTRICT_H=$(field "${REPLY[0]}" hospitals)
NODE=$(field "${REPLY[4]}" node)
ok $([ "${NODES:-0}" -gt 0 ] && echo 1 || echo 0) \
   "the daemon serves a non-empty network" "nodes=$NODES"

printf '\n%s2. dispatch over the wire%s\n' "$BOLD" "$OFF"
# DISPATCH <node> <need_hosp> <need_amb> <need_med> <med_qty> <urgency> <sla_ms> <horizon> <geom>
send "DISPATCH $NODE 1 0 0 1 3 480000 0 0"
D="${REPLY[0]}"
match "$D" '"ok":true'      "a well-formed dispatch succeeds"
match "$D" '"latency_us"'   "the reply carries its own decision latency"
match "$D" '"rejected"'     "the reply carries the rejected alternatives"
AMB=$(field "$D" amb); HOSP=$(field "$D" hosp)
ok $([ -n "$AMB" ] && [ -n "$HOSP" ] && echo 1 || echo 0) \
   "the reply names both an ambulance and a hospital" "amb=$AMB hosp=$HOSP"

send "DISPATCH $NODE 1 0 0 1 3 480000 0 1"
G="${REPLY[0]}"
match "$G" '"leg1":[[' "geom=1 returns the response leg as route geometry"
match "$G" '"leg2":[[' "geom=1 returns the transport leg as route geometry"
# Both legs are emitted incident-first, so they must share their first point.
L1=$(printf '%s' "$G" | sed -n 's/.*"leg1":\[\(\[[^]]*\]\).*/\1/p')
L2=$(printf '%s' "$G" | sed -n 's/.*"leg2":\[\(\[[^]]*\]\).*/\1/p')
ok $([ -n "$L1" ] && [ "$L1" = "$L2" ] && echo 1 || echo 0) \
   "both legs start at the incident, so the drawn route is continuous" \
   "leg1 starts $L1, leg2 starts $L2"

printf '\n%s3. malformed input is refused, not crashed on%s\n' "$BOLD" "$OFF"
send "DISPATCH 999999999 1 0 0 1 3 480000 0 0" \
     "NONSENSE 1 2 3" \
     "CLOSE 999999999" \
     "COMMIT 999999 0 0 1" \
     "" \
     "STATS"
match "${REPLY[0]}" '"bad node"'        "an out-of-range node is rejected by name"
match "${REPLY[1]}" '"unknown command"' "an unknown verb is rejected by name"
match "${REPLY[2]}" '"bad edge"'        "an out-of-range edge id is rejected by name"
match "${REPLY[3]}" '"bad id"'          "an out-of-range ambulance id is rejected by name"
match "${REPLY[5]}" '"ok":true'         "the connection survives every bad command"

printf '\n%s4. state-changing commands really change state%s\n' "$BOLD" "$OFF"
send "STATS"
BEDS0=$(field "${REPLY[0]}" beds_free); BUSY0=$(field "${REPLY[0]}" busy)
send "COMMIT $AMB $HOSP 0 1" "STATS"
match "${REPLY[0]}" '"ok":true' "COMMIT is accepted"
BEDS1=$(field "${REPLY[1]}" beds_free); BUSY1=$(field "${REPLY[1]}" busy)
ok $([ "$BEDS1" -eq $((BEDS0 - 1)) ] && echo 1 || echo 0) \
   "COMMIT takes exactly one bed" "$BEDS0 -> $BEDS1"
ok $([ "$BUSY1" -eq $((BUSY0 + 1)) ] && echo 1 || echo 0) \
   "COMMIT occupies exactly one ambulance" "$BUSY0 -> $BUSY1"

send "RELEASE $AMB $HOSP" "STATS"
BEDS2=$(field "${REPLY[1]}" beds_free); BUSY2=$(field "${REPLY[1]}" busy)
ok $([ "$BEDS2" -eq "$BEDS0" ] && [ "$BUSY2" -eq "$BUSY0" ] && echo 1 || echo 0) \
   "RELEASE returns the bed and the vehicle" "beds $BEDS2 vs $BEDS0, busy $BUSY2 vs $BUSY0"

send "CLOCK 10800000" "CLOCK 36000000"
N_NIGHT=$(field "${REPLY[0]}" docs_on_duty)
N_DAY=$(field "${REPLY[1]}" docs_on_duty)
ok $([ -n "$N_NIGHT" ] && [ -n "$N_DAY" ] && [ "$N_NIGHT" != "$N_DAY" ] && echo 1 || echo 0) \
   "CLOCK moves the roster: 03:00 and 10:00 have different staffing" \
   "night=$N_NIGHT day=$N_DAY"

send "RESTOCK 0 0 5"
match "${REPLY[0]}" '"units_added"' "RESTOCK reports how many units it actually added"

printf '\n%s5. road closures and index rebuild%s\n' "$BOLD" "$OFF"
send "CLOSE 10" "REBUILD" "OPEN 10" "REBUILD"
match "${REPLY[0]}" '"index_stale":true' "CLOSE flags the distance table as stale"
match "${REPLY[0]}" '"took_ns"'          "CLOSE reports its own O(1) cost"
match "${REPLY[1]}" '"generation"'       "REBUILD reports the new table generation"
G1=$(field "${REPLY[1]}" generation); G2=$(field "${REPLY[3]}" generation)
ok $([ -n "$G1" ] && [ -n "$G2" ] && [ "$G2" -gt "$G1" ] && echo 1 || echo 0) \
   "each rebuild advances the generation counter" "$G1 -> $G2"

printf '\n%s6. throughput under pipelining%s\n' "$BOLD" "$OFF"
CMDS=(); for i in $(seq 1 200); do CMDS+=("DISPATCH $NODE 1 0 0 1 3 480000 0 0"); done
T0=$(date +%s%N)
send "${CMDS[@]}"
T1=$(date +%s%N)
GOT=${#REPLY[@]}
ok $([ "$GOT" -eq 200 ] && echo 1 || echo 0) \
   "200 pipelined dispatches return 200 replies" "got $GOT"
SAME=1
for r in "${REPLY[@]}"; do case "$r" in *'"ok":true'*) ;; *) SAME=0 ;; esac; done
ok "$SAME" "every pipelined reply is a successful decision"
MS=$(( (T1 - T0) / 1000000 ))
printf '        200 dispatches in %s ms over one socket\n' "$MS"

printf '\n%s7. real city rosters over the wire%s\n' "$BOLD" "$OFF"
send "CITIES"
match "${REPLY[0]}" '"cities":['   "CITIES lists the compiled-in rosters"
match "${REPLY[0]}" '"active":-1'  "the engine starts on the synthetic district"
match "${REPLY[0]}" 'CC-BY 4.0'    "CITIES carries the dataset attribution"

# Load a real roster and check the swap is total: roster size, named
# hospitals, and a dispatch that still resolves afterwards.
send "CITY 0" "STATS" "HOSPITALS" "DISPATCH $NODE 1 0 0 1 3 480000 0 0"
match "${REPLY[0]}" '"ok":true'                 "CITY loads a real roster"
match "${REPLY[0]}" '"missions_invalidated"'    "CITY warns that live mission ids are stale"
match "${REPLY[1]}" '"city":0'                  "STATS reports which roster is loaded"
match "${REPLY[2]}" '"name":"'                  "HOSPITALS carries real hospital names"
match "${REPLY[2]}" '"beds_reported"'           "HOSPITALS carries the reported bed count"
match "${REPLY[2]}" '"inferred"'                "HOSPITALS flags inferred departments"
match "${REPLY[3]}" '"ok":true'                 "a dispatch resolves against the real roster"
CITY_H=$(field "${REPLY[1]}" hospitals)

send "CITY 99999"
match "${REPLY[0]}" '"ok":false' "an out-of-range city index is refused"

# ...and that going back restores the district exactly, not some hybrid.
send "DISTRICT" "STATS"
match "${REPLY[0]}" '"city":-1'  "DISTRICT returns to the synthetic world"
BACK_H=$(field "${REPLY[1]}" hospitals)
ok $([ -n "$CITY_H" ] && [ -n "$BACK_H" ] && [ "$CITY_H" != "$BACK_H" ] \
     && [ "$BACK_H" = "$DISTRICT_H" ] && echo 1 || echo 0) \
   "the roster count follows the world and restores on the way back" \
   "district=$DISTRICT_H city=$CITY_H back=$BACK_H"

printf '\n%s%d checks, %d failed%s\n' "$BOLD" "$CHECKS" "$FAILS" "$OFF"
if [ "$FAILS" -gt 0 ]; then
    printf '%sFAILURES PRESENT%s\n' "$RED" "$OFF"; exit 1
fi
printf '%sall checks passed%s\n' "$GREEN" "$OFF"
