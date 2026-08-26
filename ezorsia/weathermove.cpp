#include "stdafx.h"
#include "weather.h"
#include "weatherfx.h"
#include "debug.h"
#include "ztl/ztl.h"

// What the weather does to how the player moves: FOOTING only.
//
// Once snow has settled the ground goes slippery, and that is a change to the client's
// PHYSICS CONSTANTS rather than to the character's stats. Friction is not a stat, has no
// wire representation and nowhere else to live, so it belongs here.
//
// WALK SPEED USED TO BE HERE TOO AND IS NOT ANY MORE. It scaled walkSpeed in the same
// table, which worked but could never appear in the stat window -- that reads the
// character's speed stat, and the physics table is not it. It is a real -10 SPEED buff
// now (server/weather/WeatherDebuff.java), which slows the player AND shows, and carries
// its own icon. Both at once would have slowed a blizzard twice over.
//
// WHERE THE CONSTANTS LIVE
// ------------------------
// Client/Data/Map/Physics.img is read once at startup into a flat table of doubles. The
// loader is at 0x00A434C0; each field is read by name and stored through the accessor at
// 0x00A45D42, which is simply:
//
//     00A45D42  mov eax, dword ptr [ecx + 4]
//     00A45D45  ret
//
// and every one of its 19 loader call sites passes ecx = this+4, so what it returns is
// [CPhysicalSpace2D + 8]. CPhysicalSpace2D is the same singleton at 0x00BEBFA0 that
// weatheraccum and weatherpuddle already probe footholds against. The offsets below were
// read straight out of that loader's store instructions.
//
// NO CODE CAVE. This writes data the engine reads every frame anyway, so there is nothing
// to patch and nothing to restore on unload beyond the values themselves.
//
// WHY IT IS SAFE AGAINST THE SERVER
// ---------------------------------
// Movement is client authoritative in v83: the client walks and reports where it ended
// up. Both effects here make the player SLOWER or less controlled, never faster, so
// neither can produce a position the server would refuse. The friction floor below is
// what keeps that true: at no setting does sliding carry the player further per second
// than an ordinary walk would.
//
// EVERY VALUE WRITTEN IS ABSOLUTE, and is derived from a CAPTURED STOCK VALUE rather than
// from whatever is in the table now. Scaling what is already there would compound every
// frame and stop the player dead inside a second.

#define ADDR_PHYS_SPACE     0x00BEBFA0
// CPhysicalSpace2D + 8 -> double[]. NOT +4, which is what this said and which is not
// written by anything: the constructor at 0x00A43433 does `lea edi,[esi+4]` and then
// `mov [edi+4], ebx`, i.e. it initialises [this+8] and leaves [this+4] as uninitialised
// heap. The accessor 0x00A45D42 is `mov eax,[ecx+4]; ret` and every one of its 19 loader
// call sites passes edi (= this+4), so it returns [this+8]; the engine's own consumers
// agree, e.g. 0x009B2793 `mov eax,[0xBEBFA0]; mov ecx,[eax+8]; fcomp qword [ecx+0x78]`.
// With +4 this module read a null pointer and did nothing at all, silently, and the
// LOG_ONCE written to catch exactly that case was itself unreachable on that path.
#define PHYS_TABLE_OFF      8

// Indices into that table, in doubles, taken from the loader's own store instructions
// (0x00A43BC8 stores to [eax+0x78], 0x00A43C2B to [eax+0x80]) and matching Physics.img's
// field order.
#define PHYS_I_MAX_FRICTION 15         // +0x78, stock 2.0
#define PHYS_I_MIN_FRICTION 16         // +0x80, stock 0.05. The engine's own floor.

// maxFriction IS NOT THE FRICTION. It is only the upper bound of a clamp, applied at
// 0x009B279E to a friction computed as the product of three modifiers that are each 1.0
// on ordinary ground:
//
//     009B279E  fcomp qword ptr [ecx + 0x78]   ; friction vs maxFriction
//     009B27A4  jbe   0x9b27ac; <= : leave it alone
//     009B27A6  fld   qword ptr [ecx + 0x78]   ; >  : clamp down to maxFriction
//
// So the clamp only bites when the ceiling is BELOW the friction in force, which for a
// walking player is 1.0. The previous calibration scaled stock 2.0 by at worst 0.50,
// giving a reachable ceiling band of [1.0, 2.0] -- entirely at or above 1.0, so the
// comparison always took the `jbe` and the whole feature was inert at every setting.
//
// The target is therefore expressed in FRICTION units against that 1.0, not as a fraction
// of stock. At full accumulation the ceiling lands at SNOW_FRICTION_MIN and the player
// really does slide.
#define ORDINARY_FRICTION   1.00f      // the product on ordinary ground
// Ceiling at full accumulation, and this is as close to "no extra slide" as the engine
// allows while the footing is on at all.
//
// Effective friction is ceiling/2 once the ceiling drops under 1.0, so the reachable band
// is (0, 0.5] and there is nothing between 0.5 and 1.0. Crossing into the band at all
// costs half the friction in ONE STEP, whatever this constant says; all it controls is how
// much FURTHER the ground falls away as the drift deepens:
//
//     ceiling 0.60  ->  effective 0.30  ->  0.20 below the top of the band
//     ceiling 0.80  ->  effective 0.40  ->  0.10
//     ceiling 0.95  ->  effective 0.475 ->  0.025, which is where it now sits
//
// So the deep drift is now barely slipperier than the moment the footing arrives, and what
// the player feels is almost entirely that single onset step. That is deliberate: the step
// is the part that cannot be removed, so there is little point spending the rest of the
// band on top of it.
//
// DO NOT SET THIS TO 1.00. The clamp stops engaging at exactly 1.0 and the footing is off;
// there is no gentler setting between 0.95 and off, because the values in between are the
// ones the engine's halving cannot express.
#define SNOW_FRICTION_MIN   0.95f      // ceiling at full accumulation

// THE ENGINE HALVES ANY FRICTION BELOW 1.0, so the response to the ceiling is a STEP, not
// a ramp, and no amount of easing can smooth it:
//
//     009B27C4  fcomp qword [0xAF0DE0]   ; friction vs 1.0
//     009B27CD  jae   0x9B27DB; >= 1.0: leave it
//     009B27D2  fmul  qword [0xAF0D48]   ; <  1.0: halve it
//
// so effective friction is 1.0 for any ceiling at or above 1.0, and ceiling/2 below it.
// The reachable band is (0, 0.5]; there is nothing between 0.5 and 1.0. Rather than
// pretend otherwise, the drop is deliberately deferred until the drift is actually
// visible, and then taken in one go.
//
// Below this accumulation level the ceiling is parked AT 1.0: still effectively stock,
// but one step from the live band, so nothing happens on the first few flakes. 0.15 is
// about 13 s of snowfall and roughly ten deposits on the ground.
#define SNOW_ONSET_LEVEL    0.15f

// Per second of easing toward the target, so a sky change is felt as the weather
// arriving rather than as a step change in the controls.
#define EASE_PER_SEC        0.55f

// Never write a ceiling outside this. The floor is kept clear of the engine's own
// minFriction (index 16, stock 0.05): a ceiling below that floor is a contradiction the
// engine resolves by clamping straight back up, so it would read as the effect cutting
// out at maximum strength.
#define CLAMP_LO            0.35f
#define CLAMP_HI            2.00f

namespace {

bool   g_bHaveStock = false;
double g_dStockFriction = 0.0;

float  g_fFriction = 1.0f;
DWORD  g_dwLastFrame = 0;

double* PhysTable() {
    __try {
        unsigned char* pSpace = *reinterpret_cast<unsigned char**>(ADDR_PHYS_SPACE);
        if (!pSpace) {
            return nullptr;
        }
        return *reinterpret_cast<double**>(pSpace + PHYS_TABLE_OFF);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Stock is captured from the table itself rather than from the numbers in Physics.img,
// so a server that ships different constants keeps them.
bool CaptureStock(double* pT) {
    __try {
        const double f = pT[PHYS_I_MAX_FRICTION];
        // Sanity, so a mislocated table cannot be adopted as "stock" and then written
        // back over something that was never ours.
        if (!(f > 0.001 && f < 1000.0)) {
            LOG_ONCE("weathermove: physics table at +0x%X reads maxFriction %.3f, which is "
                     "not plausible; footing effects off", PHYS_TABLE_OFF, f);
            return false;
        }
        g_dStockFriction = f;
        g_bHaveStock = true;
        // The eased value lives in friction units and its resting target is stock, so it
        // must start there rather than at a placeholder.
        g_fFriction = (float)f;
        DEBUG_MESSAGE("weathermove: stock maxFriction %.3f", f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// fCeiling is an ABSOLUTE friction ceiling, not a scale of stock. Passing the captured
// stock value restores the client's own constant exactly.
void Write(double* pT, double dCeiling) {
    __try {
        pT[PHYS_I_MAX_FRICTION] = dCeiling;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

float Clamp(float v) {
    if (v < CLAMP_LO) return CLAMP_LO;
    if (v > CLAMP_HI) return CLAMP_HI;
    return v;
}

}  // namespace


void WeatherMove_Frame() {
    const DWORD dwNow = GetTickCount();
    DWORD dwDelta = g_dwLastFrame ? (dwNow - g_dwLastFrame) : 0;
    g_dwLastFrame = dwNow;
    if (dwDelta > 200) {
        dwDelta = 200;
    }

    double* pT = PhysTable();
    if (!pT) {
        // Loud, because this is the shape the +4/+8 offset bug took: a null table is a
        // silent no-op, and the plausibility LOG_ONCE below can never be reached from it.
        LOG_ONCE("weathermove: no physics table at [*0x%08X + 0x%X]; footing effects off",
                 ADDR_PHYS_SPACE, PHYS_TABLE_OFF);
        return;
    }
    if (!g_bHaveStock && !CaptureStock(pT)) {
        return;
    }

    // Footing follows the SETTLED SNOW, not the falling snow: it is the drift underfoot
    // that is slippery, so it arrives late in a snowfall (later still, by SNOW_ONSET_LEVEL)
    // and outlasts it.
    //
    // Read from the DRIFT rather than from the sky, which is what makes "outlasts it"
    // true. Gating on Weather::CurrentSky() snapped the friction home the instant the sky
    // changed, roughly 24 seconds before the last drift had melted, so the behaviour this
    // comment describes never actually happened; and because the level was not qualified
    // by kind, a map full of settled cherry petals drove snow footing.
    const float fLevel = WeatherAccum_SnowLevel();

    // The target CEILING, in friction units.
    //
    //   level 0                 -> the client's own stock value, so nothing that
    //                              legitimately runs above 1.0 friction is touched
    //   0 < level < ONSET       -> parked at 1.0: still effectively stock (the clamp is
    //                              never engaged at exactly 1.0), but one step from the
    //                              live band
    //   level >= ONSET          -> ramps 1.0 down to SNOW_FRICTION_MIN
    //
    // The traverse from stock 2.0 to 1.0 really is invisible, because everything at or
    // above 1.0 clamps nothing. Crossing BELOW 1.0 is not invisible and cannot be made so:
    // see SNOW_ONSET_LEVEL, the engine halves at that boundary. That single step is the
    // footing arriving, and it is deferred to a point where there is visibly snow to
    // explain it.
    float fCeilingWant = (float)g_dStockFriction;
    if (fLevel > 0.0f) {
        if (fLevel < SNOW_ONSET_LEVEL) {
            fCeilingWant = ORDINARY_FRICTION;
        } else {
            const float t = (fLevel - SNOW_ONSET_LEVEL) / (1.0f - SNOW_ONSET_LEVEL);
            fCeilingWant = ORDINARY_FRICTION - (ORDINARY_FRICTION - SNOW_FRICTION_MIN) * t;
        }
    }

    // Eased in friction units. Below 1.0 the ease is genuinely continuous in what the
    // player feels, because eff = ceiling/2 there; the ease across [1.0, stock] is a
    // no-op that simply delays the onset slightly, which is wanted.
    const float fStep = EASE_PER_SEC * ((float)dwDelta / 1000.0f);
    if (g_fFriction < fCeilingWant) {
        g_fFriction = (g_fFriction + fStep > fCeilingWant) ? fCeilingWant : g_fFriction + fStep;
    } else if (g_fFriction > fCeilingWant) {
        g_fFriction = (g_fFriction - fStep < fCeilingWant) ? fCeilingWant : g_fFriction - fStep;
    }

    Write(pT, (double)Clamp(g_fFriction));
}


// Put the client's own constants back. Called on field teardown, on unload, and every
// frame on a field that has no falling sky, because a table left scaled would follow the
// player into a map with no weather at all, and out of the game into the next session's
// login screen. Idempotent: it writes the captured stock value, so the per-frame caller
// costs one store on an already-stock table.
void WeatherMove_Restore() {
    if (!g_bHaveStock) {
        return;
    }
    double* pT = PhysTable();
    if (pT) {
        Write(pT, g_dStockFriction);
    }
    g_fFriction = (float)g_dStockFriction;
    g_dwLastFrame = 0;
}
