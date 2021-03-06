/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2013--2015
*/

/*
 Control/model for TRV and boiler.
 */
#include <util/atomic.h>

#include "V0p2_Main.h"

#include "Control.h"

#include "EEPROM_Utils.h"
#include "FHT8V_Wireless_Rad_Valve.h"
#include "PRNG.h"
#include "Power_Management.h"
#include "RFM22_Radio.h"
#include "RTC_Support.h"
#include "Security.h"
#include "Serial_IO.h"
#include "Schedule.h"
#include "UI_Minimal.h"


// If true then is in WARM (or BAKE) mode; defaults to (starts as) false/FROST.
// Should be only be set when 'debounced'.
// Defaults to (starts as) false/FROST.
static bool isWarmMode;
// If true then the unit is in 'warm' (heating) mode, else 'frost' protection mode.
bool inWarmMode() { return(isWarmMode); }
// Has the effect of forcing the warm mode to the specified state immediately.
// Should be only be called once 'debounced' if coming from a button press for example.
// If forcing to FROST mode then any pending BAKE time is cancelled.
void setWarmModeDebounced(const bool warm)
  {
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Call to setWarmModeDebounced(");
  DEBUG_SERIAL_PRINT(warm);
  DEBUG_SERIAL_PRINT_FLASHSTRING(")");
  DEBUG_SERIAL_PRINTLN();
#endif
  isWarmMode = warm;
#ifdef SUPPORT_BAKE
  if(!warm) { cancelBakeDebounced(); }
#endif
  }


#ifdef SUPPORT_BAKE // IF DEFINED: this unit supports BAKE mode.
// Only relevant if isWarmMode is true,
static uint_least8_t bakeCountdownM;
// If true then the unit is in 'BAKE' mode, a subset of 'WARM' mode which boosts the temperature target temporarily.
bool inBakeMode() { return(isWarmMode && (0 != bakeCountdownM)); }
// Should be only be called once 'debounced' if coming from a button press for example.
// Cancel 'bake' mode if active; does not force to FROST mode.
void cancelBakeDebounced() { bakeCountdownM = 0; }
// Start/restart 'BAKE' mode and timeout.
// Should be only be called once 'debounced' if coming from a button press for example.
void startBakeDebounced() { isWarmMode = true; bakeCountdownM = BAKE_MAX_M; }
#endif





#if defined(UNIT_TESTS)
// Support for unit tests to force particular apparent WARM setting (without EEPROM writes).
//enum _TEST_basetemp_override
//  {
//    _btoUT_normal = 0, // No override
//    _btoUT_min, // Minimum settable/reasonable temperature.
//    _btoUT_mid, // Medium settable/reasonable temperature.
//    _btoUT_max, // Minimum settable/reasonable temperature.
//  };
// Current override state; 0 (default) means no override.
static _TEST_basetemp_override _btoUT_override;
// Set the override value (or remove the override).
void _TEST_set_basetemp_override(const _TEST_basetemp_override override)
  { _btoUT_override = override; }
#endif


#define TEMP_SCALE_MIN (BIASECO_WARM-1) // Bottom of range for adjustable-base-temperature systems.
#define TEMP_SCALE_MID ((BIASECO_WARM + BIASCOM_WARM + 1)/2) // Middle of range for adjustable-base-temperature systems; should be 'eco' baised.
#define TEMP_SCALE_MAX (BIASCOM_WARM+1) // Top of range for adjustable-base-temperature systems.

// If true (the default) then the system has an 'Eco' energy-saving bias, else it has a 'comfort' bias.
// Several system parameters are adjusted depending on the bias,
// with 'eco' slanted toward saving energy, eg with lower target temperatures and shorter on-times.
#ifndef hasEcoBias // If not a macro...
// True if WARM temperature at/below halfway mark between eco and comfort levels.
// Midpoint should be just in eco part to provide a system bias toward eco.
bool hasEcoBias() { return(getWARMTargetC() <= TEMP_SCALE_MID); }
//#endif
#endif

// Get 'FROST' protection target in C; no higher than getWARMTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
#if defined(TEMP_POT_AVAILABLE)
// Derived from temperature pot position.
uint8_t getFROSTTargetC()
  {
  // Prevent falling to lowest frost temperature if relative humidity is high (eg to avoid mould).
  const uint8_t result = (!hasEcoBias() || (RelHumidity.isAvailable() && RelHumidity.isRHHighWithHyst())) ? BIASCOM_FROST : BIASECO_FROST;
#if defined(SETTABLE_TARGET_TEMPERATURES)
  const uint8_t stored = eeprom_read_byte((uint8_t *)EE_START_FROST_C);
  // If stored value is set and in bounds and higher than computed value then use stored value instead.
  if((stored >= MIN_TARGET_C) && (stored <= MAX_TARGET_C) && (stored > result)) { return(stored); }
#endif
  return(result);
  }
#elif defined(SETTABLE_TARGET_TEMPERATURES)
// Note that this value is non-volatile (stored in EEPROM).
uint8_t getFROSTTargetC()
  {
  // Get persisted value, if any.
  const uint8_t stored = eeprom_read_byte((uint8_t *)EE_START_FROST_C);
  // If out of bounds or no stored value then use default.
  if((stored < MIN_TARGET_C) || (stored > MAX_TARGET_C)) { return(FROST); }
  // TODO-403: cannot use hasEcoBias() with RH% as that would cause infinite recursion!
  // Return valid persisted value.
  return(stored);
  }
#else
#define getFROSTTargetC() (FROST) // Fixed value.
#endif

// Get 'WARM' target in C; no lower than getFROSTTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
#if defined(TEMP_POT_AVAILABLE)
// Derived from temperature pot position, 0 for coldest (most eco), 255 for hottest (comfort).
// Temp ranges from eco-1C to comfort+1C levels across full (reduced jitter) [0,255] pot range.
// May be fastest computing values at the extreme ends of the range.
// Exposed for unit testing.
uint8_t computeWARMTargetC(const uint8_t pot)
  {
#if defined(V0p2_REV)
#if 7 == V0p2_REV // Must match DORM1 scale 7 position scale 16|17|18|19|20|21|22 with frost/boost at extremes.
#if (16 != TEMP_SCALE_MIN) || (22 != TEMP_SCALE_MAX)
#error Temperature scale must run from 16 to 22 inclusive for REV7 / DORM1 unit.
#endif
#endif
#endif
  const uint8_t range = TEMP_SCALE_MAX - TEMP_SCALE_MIN + 1;
  const uint8_t band = 256 / range; // Width of band for each degree C...

  // If there are is relatively small number of distinct temperature values
  // then compute result iteratively...
  if(pot >= 256 - band) { return(TEMP_SCALE_MAX); } // At top... (optimisation / robustness)
  if(pot < band) { return(TEMP_SCALE_MIN); } // At bottom... (optimisation / robustness)
  if(range < 10)
    {
    uint8_t result = TEMP_SCALE_MIN+1;
    for(uint8_t ppot = band<<1; ppot < pot; ++result) { ppot += band; }
    return(result);
    }
  // ...else do it in one step with a division.
  return((pot / band) + TEMP_SCALE_MIN); // Intermediate (requires expensive run-time division).
  }

// Exposed implementation.
// Uses cache to avoid expensive recomputation.
// NOT safe in face of interrupts.
uint8_t getWARMTargetC()
  {
#if defined(UNIT_TESTS)
  // Special behaviour for unit tests.
  switch(_btoUT_override)
    {
    case _btoUT_min: return(TEMP_SCALE_MIN);
    case _btoUT_mid: return(TEMP_SCALE_MID);
    case _btoUT_max: return(TEMP_SCALE_MAX);
    }
#endif

  const uint8_t pot = TempPot.get();

  // Cached input and result values; initially zero.
  static uint8_t potLast;
  static uint8_t resultLast;
  // Force recomputation if pot value changed
  // or apparently no calc done yet (unlikely/impossible zero cached result).
  if((potLast != pot) || (0 == resultLast))
    {
    const uint8_t result = computeWARMTargetC(pot);
    // Cache input/result.
    resultLast = result;
    potLast = pot;
    return(result);
    }

  // Return cached result.
  return(resultLast);
  }
#elif defined(SETTABLE_TARGET_TEMPERATURES)
// Note that this value is non-volatile (stored in EEPROM).
uint8_t getWARMTargetC()
  {
#if defined(UNIT_TESTS)
  // Special behaviour for unit tests.
  switch(_btoUT_override)
    {
    case _btoUT_min: return(TEMP_SCALE_MIN);
    case _btoUT_mid: return(TEMP_SCALE_MID);
    case _btoUT_max: return(TEMP_SCALE_MAX);
    }
#endif

  // Get persisted value, if any.
  const uint8_t stored = eeprom_read_byte((uint8_t *)EE_START_WARM_C);
  // If out of bounds or no stored value then use default (or frost value if set and higher).
  if((stored < MIN_TARGET_C) || (stored > MAX_TARGET_C)) { return(fnmax((uint8_t)WARM, getFROSTTargetC())); }
  // Return valid persisted value (or frost value if set and higher).
  return(fnmax(stored, getFROSTTargetC()));
  }
#else
#define getWARMTargetC() ((uint8_t) (WARM)) // Fixed value.
#endif

#if defined(SETTABLE_TARGET_TEMPERATURES)
// Set (non-volatile) 'FROST' protection target in C; no higher than getWARMTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
// Can also be used, even when a temperature pot is present, to set a floor setback temperature.
// Returns false if not set, eg because outside range [MIN_TARGET_C,MAX_TARGET_C], else returns true.
bool setFROSTTargetC(uint8_t tempC)
  {
  if((tempC < MIN_TARGET_C) || (tempC > MAX_TARGET_C)) { return(false); } // Invalid temperature.
  if(tempC > getWARMTargetC()) { return(false); } // Cannot set above WARM target.
  eeprom_smart_update_byte((uint8_t *)EE_START_FROST_C, tempC); // Update in EEPROM if necessary.
  return(true); // Assume value correctly written.
  }
#endif
#if defined(SETTABLE_TARGET_TEMPERATURES) && !defined(TEMP_POT_AVAILABLE)
// Set 'WARM' target in C; no lower than getFROSTTargetC() returns, strictly positive, in range [MIN_TARGET_C,MAX_TARGET_C].
// Returns false if not set, eg because below FROST setting or outside range [MIN_TARGET_C,MAX_TARGET_C], else returns true.
bool setWARMTargetC(uint8_t tempC)
  {
  if((tempC < MIN_TARGET_C) || (tempC > MAX_TARGET_C)) { return(false); } // Invalid temperature.
  if(tempC < getFROSTTargetC()) { return(false); } // Cannot set below FROST target.
  eeprom_smart_update_byte((uint8_t *)EE_START_WARM_C, tempC); // Update in EEPROM if necessary.
  return(true); // Assume value correctly written.
  }
#endif


#ifndef getMinBoilerOnMinutes
// Get minimum on (and off) time for pointer (minutes); zero if not in hub mode.
uint8_t getMinBoilerOnMinutes() { return(~eeprom_read_byte((uint8_t *)EE_START_MIN_BOILER_ON_MINS_INV)); }
#endif

#ifndef setMinBoilerOnMinutes
// Set minimum on (and off) time for pointer (minutes); zero to disable hub mode.
// Suggested minimum of 4 minutes for gas combi; much longer for heat pumps for example.
void setMinBoilerOnMinutes(uint8_t mins) { eeprom_smart_update_byte((uint8_t *)EE_START_MIN_BOILER_ON_MINS_INV, ~(mins)); }
#endif

// Minimum slew/error % distance in central range; should be larger than smallest temperature-sensor-driven step (6) to be effective; [1,100].
// Note: keeping TRV_MIN_SLEW_PC sufficiently high largely avoids spurious hunting back and forth from single-ulp noise.
#ifndef TRV_MIN_SLEW_PC
#define TRV_MIN_SLEW_PC 7
#endif
// Set maximum valve slew rate (percent/minute) when close to target temperture.
// Note: keeping TRV_MAX_SLEW_PC_PER_MIN small reduces noise and overshoot and surges of water
// (eg for when additionally charged by the m^3 of flow in district heating systems)
// and will likely work better with high-thermal-mass / slow-response systems such as UFH.
// Should be << 100%/min, and probably << 30%/min, given that 30% may be the effective control range of many rad valves.
#ifndef TRV_MAX_SLEW_PC_PER_MIN
#define TRV_MIN_SLEW_PC_PER_MIN 1 // Minimal slew rate (%/min) to keep flow rates as low as possible.
#ifndef TRV_SLEW_GLACIAL
#define TRV_MAX_SLEW_PC_PER_MIN 5 // Maximum normal slew rate (%/min), eg to fully open from off when well under target; [1,100].
#else
#define TRV_MAX_SLEW_PC_PER_MIN TRV_MIN_SLEW_PC_PER_MIN
#endif
#endif

// Derived from basic slew values.
#ifndef TRV_SLEW_GLACIAL
#define TRV_SLEW_PC_PER_MIN_VFAST (min(34,(4*TRV_MAX_SLEW_PC_PER_MIN))) // Takes >= 3 minutes for full travel.
#define TRV_SLEW_PC_PER_MIN_FAST (min(20,(2*TRV_MAX_SLEW_PC_PER_MIN))) // Takes >= 5 minutes for full travel.
#else
#define TRV_SLEW_PC_PER_MIN_FAST TRV_MAX_SLEW_PC_PER_MIN
#define TRV_SLEW_PC_PER_MIN_VFAST TRV_MAX_SLEW_PC_PER_MIN
#endif




#ifdef OCCUPANCY_SUPPORT
// Singleton implementation for entire node.
OccupancyTracker Occupancy;

// Crude percentage occupancy confidence [0,100].
// Returns 0 if unknown or known unoccupied.
#if (OCCUPATION_TIMEOUT_M < 25) || (OCCUPATION_TIMEOUT_M > 100)
#error needs support for different occupancy timeout
#elif OCCUPATION_TIMEOUT_M <= 25
#define OCCCP_SHIFT 2
#elif OCCUPATION_TIMEOUT_M <= 50
#define OCCCP_SHIFT 1
#elif OCCUPATION_TIMEOUT_M <= 100
#define OCCCP_SHIFT 0
#endif

// Update notion of occupancy confidence.
uint8_t OccupancyTracker::read()
  {
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    // Compute as percentage.
    const uint8_t newValue = (0 == occupationCountdownM) ? 0 :
        fnmin((uint8_t)((uint8_t)100 - (uint8_t)((((uint8_t)OCCUPATION_TIMEOUT_M) - occupationCountdownM) << OCCCP_SHIFT)), (uint8_t)100);
    value = newValue;
    // Run down occupation timer (or run up vacancy time) if need be.
    if(occupationCountdownM > 0) { --occupationCountdownM; vacancyM = 0; vacancyH = 0; }
    else if(vacancyH < 0xffU) { if(++vacancyM >= 60) { vacancyM = 0; ++vacancyH; } }
    // Run down 'recent activity' timer.
    if(activityCountdownM > 0) { --activityCountdownM; }
    return(value);
    }
  }

// Call when some/weak evidence of room occupation, such as a light being turned on, or voice heard.
// Do not call based on internal/synthetic events.
// Doesn't force the room to appear recently occupied.
// If the hardware allows this may immediately turn on the main GUI LED until normal GUI reverts it,
// at least periodically.
// Probably do not call on manual control operation to avoid interfering with UI operation.
// Thread-safe.
void OccupancyTracker::markAsPossiblyOccupied()
  {
//  if(0 == activityCountdownM) // Only flash the UI at start of external activity to limit flashing.
    { LED_HEATCALL_ON_ISR_SAFE(); }
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    occupationCountdownM = fnmax((uint8_t)occupationCountdownM, (uint8_t)(OCCUPATION_TIMEOUT_1_M));
    }
  activityCountdownM = 2;
  }
#endif




// Returns true iff there is a full set of stats (none unset) and this 3/4s of the values are higher than the supplied sample.
// Always returns false if all samples are the same.
//   * s is start of (24) sample set in EEPROM
//   * sample to be tested for being in lower quartile
bool inBottomQuartile(const uint8_t *sE, const uint8_t sample)
  {
  uint8_t valuesHigher = 0;
  for(int8_t hh = 24; --hh >= 0; ++sE)
    {
    const uint8_t v = eeprom_read_byte(sE); 
    if(STATS_UNSET_INT == v) { return(false); } // Abort if not a full set of stats (eg at least one full day's worth). 
    if(v > sample) { if(++valuesHigher >= 18) { return(true); } } // Stop as soon as known to be in lower quartile.
    }
  return(false); // Not in lower quartile.
  }

// Returns true iff there is a full set of stats (none unset) and this 3/4s of the values are lower than the supplied sample.
// Always returns false if all samples are the same.
//   * s is start of (24) sample set in EEPROM
//   * sample to be tested for being in lower quartile
bool inTopQuartile(const uint8_t *sE, const uint8_t sample)
  {
  uint8_t valuesLower = 0;
  for(int8_t hh = 24; --hh >= 0; ++sE)
    {
    const uint8_t v = eeprom_read_byte(sE); 
    if(STATS_UNSET_INT == v) { return(false); } // Abort if not a full set of stats (eg at least one full day's worth). 
    if(v < sample) { if(++valuesLower >= 18) { return(true); } } // Stop as soon as known to be in upper quartile.
    }
  return(false); // Not in upper quartile.
  }

// Returns true if specified hour is (conservatively) in the specifed outlier quartile for the specified stats set.
// Returns false if a full set of stats not available, eg including the specified hour.
// Always returns false if all samples are the same.
//   * inTop  test for membership of the top quartile if true, bottom quartile if false
//   * statsSet  stats set number to use.
//   * hour  hour of day to use or ~0 for current hour.
bool inOutlierQuartile(const uint8_t inTop, const uint8_t statsSet, const uint8_t hour)
  {
  if(statsSet >= EE_STATS_SETS) { return(false); } // Bad stats set number, ie unsafe.
  const uint8_t hh = (hour > 23) ? getHoursLT() : hour;
  const uint8_t *ss = (uint8_t *)(EE_STATS_START_ADDR(statsSet));
  const uint8_t sample = eeprom_read_byte(ss + hh);
  if(STATS_UNSET_INT == sample) { return(false); }
  if(inTop) { return(inTopQuartile(ss, sample)); }
  return(inBottomQuartile(ss, sample));
  }

#ifdef ENABLE_ANTICIPATION
// Returns true iff room likely to be occupied and need warming at the specified hour's sample point based on collected stats.
// Used for predictively warming a room in smart mode and for choosing setback depths.
// Returns false if no good evidence to warm the room at the given time based on past history over about one week.
//   * hh hour to check for predictive warming [0,23]
bool shouldBeWarmedAtHour(const uint_least8_t hh)
  {
#ifndef OMIT_MODULE_LDROCCUPANCYDETECTION
  // Return false immediately if the sample hour's historic ambient light level falls in the bottom quartile (or is zero).
  // Thus aim to shave off 'smart' warming for at least 25% of the daily cycle.
  if(inOutlierQuartile(false, EE_STATS_SET_AMBLIGHT_BY_HOUR_SMOOTHED, hh)) { return(false); }
#endif

#ifdef OCCUPANCY_SUPPORT
  // Return false immediately if the sample hour's historic occupancy level falls in the bottom quartile (or is zero).
  // Thus aim to shave off 'smart' warming for at least 25% of the daily cycle.
  if(inOutlierQuartile(false, EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED, hh)) { return(false); }
#endif

  const uint8_t warmHistory = eeprom_read_byte((uint8_t *)(EE_STATS_START_ADDR(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK) + hh));
  if(0 == (0x80 & warmHistory)) // This hour has a history.
    {
//    // Return false immediately if no WARM mode this hour for the last week (ie the unit needs reminding at least once per week).
//    if(0 == warmHistory) // No explicit WARM for a week at this hour, prevents 'smart' warming.
//      { return(false); }
    // Return true immediately if this hour was in WARM mode yesterday or a week ago, and at least one other day.
    if((0 != (0x41 & warmHistory)) && (0 != (0x3e & warmHistory)))
      { return(true); }
    }

  // Return true if immediately the sample hour is usually warm, ie at or above WARM target.
  const int smoothedTempHHNext = expandTempC16(eeprom_read_byte((uint8_t *)(EE_STATS_START_ADDR(EE_STATS_SET_TEMP_BY_HOUR_SMOOTHED) + hh)));
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Smoothed C for ");
  DEBUG_SERIAL_PRINT(hh);
  DEBUG_SERIAL_PRINT_FLASHSTRING("h is ");
  DEBUG_SERIAL_PRINT(smoothedTempHHNext >> 4);
  DEBUG_SERIAL_PRINTLN();
#endif
  if((STATS_UNSET_INT != smoothedTempHHNext) && (((smoothedTempHHNext+8)>>4) >= getWARMTargetC()))
    { return(true); }

  // No good evidence for room to be warmed for specified hour.
  return(false);
  }
#endif




// Internal model of controlled radidator valve position.
ModelledRadValve NominalRadValve;

// Cache initially unset.
uint8_t ModelledRadValve::mVPRO_cache = 0;

// Return minimum valve percentage open to be considered actually/significantly open; [1,100].
// At the boiler hub this is also the threshold percentage-open on eavesdropped requests that will call for heat.
// If no override is set then DEFAULT_MIN_VALVE_PC_REALLY_OPEN is used.
// NOTE: raising this value temporarily (and shutting down the boiler immediately if possible) is one way to implement dynamic demand.
uint8_t ModelledRadValve::getMinValvePcReallyOpen()
  {
  if(0 != mVPRO_cache) { return(mVPRO_cache); } // Return cached value if possible.
  const uint8_t stored = eeprom_read_byte((uint8_t *)EE_START_MIN_VALVE_PC_REALLY_OPEN);
  const uint8_t result = ((stored > 0) && (stored <= 100)) ? stored : DEFAULT_MIN_VALVE_PC_REALLY_OPEN;
  mVPRO_cache = result; // Cache it.
  return(result);
  }

// Set and cache minimum valve percentage open to be considered really open.
// Applies to local valve and, at hub, to calls for remote calls for heat.
// Any out-of-range value (eg >100) clears the override and DEFAULT_MIN_VALVE_PC_REALLY_OPEN will be used.
void ModelledRadValve::setMinValvePcReallyOpen(const uint8_t percent)
  {
  if((percent > 100) || (percent == 0) || (percent == DEFAULT_MIN_VALVE_PC_REALLY_OPEN))
    {
    // Bad / out-of-range / default value so erase stored value if not already so.
    eeprom_smart_erase_byte((uint8_t *)EE_START_MIN_VALVE_PC_REALLY_OPEN);
    // Cache logical default value.
    mVPRO_cache = DEFAULT_MIN_VALVE_PC_REALLY_OPEN;
    return;
    }
  // Store specified value with as low wear as possible.
  eeprom_smart_update_byte((uint8_t *)EE_START_MIN_VALVE_PC_REALLY_OPEN, percent);
  // Cache it.
  mVPRO_cache = percent;
  }

// True if the controlled physical valve is thought to be at least partially open right now.
// If multiple valves are controlled then is this true only if all are at least partially open.
// Used to help avoid running boiler pump against closed valves.
// The default is to use the check the current computed position
// against the minimum open percentage.
bool ModelledRadValve::isControlledValveReallyOpen() const
  {
  if(isRecalibrating()) { return(false); }
#ifdef USE_MODULE_FHT8VSIMPLE
  if(!FHT8VisControlledValveOpen()) { return(false); }
#endif
  return(value >= getMinPercentOpen());
  }
  
// Returns true if (re)calibrating/(re)initialising/(re)syncing.
// The target valve position is not lost while this is true.
// By default there is no recalibration step.
bool ModelledRadValve::isRecalibrating() const
  {
#ifdef USE_MODULE_FHT8VSIMPLE
  if(!isSyncedWithFHT8V()) { return(true); }
#endif
  return(false);
  }

// If possible exercise the valve to avoid pin sticking and recalibrate valve travel.
// Default does nothing.
void ModelledRadValve::recalibrate()
  {
#ifdef USE_MODULE_FHT8VSIMPLE
  FHT8VSyncAndTXReset(); // Should this be decalcinate instead/also/first?
#endif
  }

// Offset from raw temperature to get reference temperature in C/16.
static const int8_t refTempOffsetC16 = 8;

//// Maximum raw temperature overshoot allowed before taking drastic action, eg simulating glacial mode.
//static const int maxRawOvershootC16 = 8 + refTempOffsetC16; // 0.5C max raw overshoot.

// Calculate reference temperature from real temperature.
// Proportional temperature regulation is in a 1C band.
// By default, for a given target XC the rad is off at (X+1)C so temperature oscillates around that point.
// This routine shifts the reference point at which the rad is off to (X+0.5C)
// ie to the middle of the specified degree, which is more intuitive,
// and which may save a little energy if users target the specified temperatures.
// Suggestion c/o GG ~2014/10 code, and generally less misleading anyway!
void ModelledRadValveInputState::setReferenceTemperatures(const int currentTempC16)
  {
  const int referenceTempC16 = currentTempC16 + refTempOffsetC16; // TODO-386: push targeted temperature down by 0.5C to middle of degree.
  refTempC16 = referenceTempC16;
  }

// Compute target temperature (stateless).
// Can be called as often as required though may be slow/expensive.
// Will be called by computeCallForHeat().
// One aim is to allow reasonable energy savings (10--30%)
// even if the device is left in WARM mode all the time,
// using occupancy/light/etc to determine when temperature can be set back
// without annoying users.
uint8_t ModelledRadValve::computeTargetTemp()
  {
  // In FROST mode.
  if(!inWarmMode())
    {
    const uint8_t frostC = getFROSTTargetC();

    // If scheduled WARM is due soon then ensure that room is at least at setback temperature
    // to give room a chance to hit the target, and for furniture and surfaces to be warm, etc.
    // Don't do this if the room has been vacant for a long time (eg so as to avoid pre-warm being higher than WARM ever).
    // Don't do this if there has been recent manual intervention, eg to allow manual 'cancellation' of pre-heat (TODO-464).
    // Only do this if the target WARM temperature is NOT an 'eco' temperature (ie very near the bottom of the scale).
    if(!Occupancy.longVacant() && isAnyScheduleOnWARMSoon() && !recentUIControlUse())
      {
      const uint8_t warmTarget = getWARMTargetC();
      // Compute putative pre-warm temperature...
      const uint8_t preWarmTempC = fnmax((uint8_t)(warmTarget - (hasEcoBias() ? SETBACK_ECO : SETBACK_DEFAULT)), frostC);
      if((frostC < preWarmTempC) && (!isEcoTemperature(warmTarget)))
        { return(preWarmTempC); }
      }

    // Apply FROST safety target temperature by default in FROST mode.
    return(frostC);
    }

#ifdef SUPPORT_BAKE
  else if(inBakeMode()) // If in BAKE mode then use elevated target.
    {
    return(fnmin((uint8_t)(getWARMTargetC() + BAKE_UPLIFT), (uint8_t)MAX_TARGET_C)); // No setbacks apply in BAKE mode.
    }
#endif

  else // In 'WARM' mode with possible setback.
    {
    const uint8_t wt = getWARMTargetC();

    // Set back target the temperature a little if the room seems to have been vacant for a long time (TODO-107)
    // or it is too dark for anyone to be active or the room is not likely occupied at this time
    //   AND no WARM schedule is active now (TODO-111)
    //   AND no recent manual interaction with the unit's local UI (TODO-464) indicating local settings override.
    // Note that this mainly has to work in domestic settings in winter (with ~8h of daylight)
    // but should also work in artificially-lit offices (maybe ~12h continuous lighting).
    // No 'lights-on' signal for a whole day is a fairly strong indication that the heat can be turned down.
    // TODO-451: TODO-453: ignore a short lights-off, eg from someone briefly leaving room or a transient shadow.
    // TODO: consider bottom quartile of amblient light as alternative setback trigger for near-continuously-lit spaces (aiming to spot daylight signature).
    const bool longLongVacant = Occupancy.longLongVacant();
    const bool longVacant = longLongVacant || Occupancy.longVacant();
    const bool notLikelyOccupiedSoon = longLongVacant || (Occupancy.isLikelyUnoccupied() && inOutlierQuartile(false, EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED));
    if(longVacant ||
       ((notLikelyOccupiedSoon || (AmbLight.getDarkMinutes() > 10)) && !isAnyScheduleOnWARMNow() && !recentUIControlUse()))
      {
      // Use a default minimal non-annoying setback if in comfort mode
      //   or if the room is likely occupied now
      //   or if the room is lit and hasn't been vacant for a very long time (TODO-107)
      //   or if the room is commonly occupied at this time and hasn't been vacant for a very long time
      //   or if a scheduled WARM period is due soon and the room hasn't been vacant for a moderately long time,
      // else a bigger 'eco' setback
      // unless an even bigger 'full' setback if the room has been vacant for a very long time
      //   or is unlikely to be unoccupied at this time of day and the target WARM temperature is at the 'eco' end.
      const uint8_t setback = (!hasEcoBias() ||
                               Occupancy.isLikelyOccupied() ||
                               (!longLongVacant && AmbLight.isRoomLit()) ||
                               (!longLongVacant && inOutlierQuartile(true, EE_STATS_SET_OCCPC_BY_HOUR_SMOOTHED)) ||
                               (!longVacant && isAnyScheduleOnWARMSoon())) ?
              SETBACK_DEFAULT :
          ((longLongVacant || (notLikelyOccupiedSoon && isEcoTemperature(wt))) ?
              SETBACK_FULL : SETBACK_ECO);

      return(fnmax((uint8_t)(wt - setback), getFROSTTargetC())); // Target must never be set low enough to create a frost/freeze hazard.
      }
    // Else use WARM target as-is.
    return(wt);
    }
  }

// TODO-467: if defined then slow to glacial on when wide deadband has been specified implying reduced heating effort.
#define GLACIAL_ON_WITH_WIDE_DEADBAND

// Computes optimal valve position given supplied input state including current position; [0,100].
// Uses no state other than that passed as the arguments (thus unit testable).
// This supplied 'retained' state may be updated.
// Uses hysteresis and a proportional control and some other cleverness.
// Is always willing to turn off quickly, but on slowly (AKA "slow start" algorithm),
// and tries to eliminate unnecessary 'hunting' which makes noise and uses actuator energy.
// Nominally called at a regular rate, once per minute.
// All inputState values should be set to sensible values before starting.
// Usually called by tick() which does required state updates afterwards.
uint8_t ModelledRadValve::computeRequiredTRVPercentOpen(const uint8_t valvePCOpen, const struct ModelledRadValveInputState &inputState, struct ModelledRadValveState &retainedState)
  {
#if 0 && defined(DEBUG)
DEBUG_SERIAL_PRINT_FLASHSTRING("targ=");
DEBUG_SERIAL_PRINT(inputState.targetTempC);
DEBUG_SERIAL_PRINT_FLASHSTRING(" room=");
DEBUG_SERIAL_PRINT(inputState.refTempC);
DEBUG_SERIAL_PRINTLN();
#endif

  // Possibly-adjusted and.or smoothed temperature to use for targetting.
  const int adjustedTempC16 = retainedState.isFiltering ? (retainedState.getSmoothedRecent() + refTempOffsetC16) : inputState.refTempC16;
  const int8_t adjustedTempC = (adjustedTempC16 >> 4);

  // (Well) under temp target: open valve up.
  if(adjustedTempC < inputState.targetTempC)
    {
//DEBUG_SERIAL_PRINTLN_FLASHSTRING("under temp");
    // Limit valve open slew to help minimise overshoot and actuator noise.
    // This should also reduce nugatory setting changes when occupancy (etc) is fluctuating.
    // Thus it may take several minutes to turn the radiator fully on,
    // though probably opening the first ~33% will allow near-maximum heat output in practice.
    if(valvePCOpen < inputState.maxPCOpen)
      {
#if defined(SUPPORT_BAKE)
      // If room is well below target and in BAKE mode then immediately open to maximum.
      // Need debounced bake mode value to avoid spurious slamming open of the valve as the user cycles through modes.
      if(inputState.inBakeMode) { return(inputState.maxPCOpen); }
#endif

      // Reduce valve hunting: defer re-opening if recently closed.
      if(retainedState.dontTurnup()) { return(valvePCOpen); }

      // Open glacially if explicitly requested or if temperature overshoot has happened or is a danger,
      // or if there's likely no one going to care about getting on target particularly quickly (or would prefer reduced noise).
      //
      // If already at least the expected minimum % open for significant flow,
      //   and wide deadband (eg in FROST mode or dark) to avoid over-eager pre-warm / anticipation for example (TODO-467)
      // OR
      //   filtering is on indicating rapid recent changes or jitter,
      //   and the last raw change was upwards,
      // then force glacial mode to try to damp oscillations and avoid overshoot and excessive valve movement (TODO-453).
      const bool beGlacial = inputState.glacial ||
          ((valvePCOpen >= inputState.minPCOpen) &&
              (
#if defined(GLACIAL_ON_WITH_WIDE_DEADBAND)
               // Don't work so hard to reach and hold target temp with wide deadband
               // (widened eg because room is dark, or this is a pre-warm in FROST mode, or temperature is gyrating)
               // and not comfort mode nor massively below target temp.
               (inputState.widenDeadband && inputState.hasEcoBias && (adjustedTempC >= (uint8_t)fnmax(inputState.targetTempC-(int)SETBACK_FULL, (int)MIN_TARGET_C))) ||
#endif
               (retainedState.isFiltering && (retainedState.getRawDelta() > 0)))); // FIXME: maybe redundant w/ GLACIAL_ON_WITH_WIDE_DEADBAND and widenDeadband set when isFiltering is true 
      if(beGlacial) { return(valvePCOpen + 1); }

      // Ensure that the valve opens quickly from cold for acceptable response.
      // Less fast if already moderately open or in the degree below target.
      const uint8_t slewRate =
          ((valvePCOpen >= DEFAULT_VALVE_PC_MODERATELY_OPEN) || (adjustedTempC == inputState.targetTempC-1)) ?
              TRV_MAX_SLEW_PC_PER_MIN : TRV_SLEW_PC_PER_MIN_FAST;
      const uint8_t minOpenFromCold = fnmax(slewRate, inputState.minPCOpen);
      // Open to 'minimum' likely open state immediately if less open currently.
      if(valvePCOpen < minOpenFromCold) { return(minOpenFromCold); }
      // Slew open relatively gently...
      return(fnmin((uint8_t)(valvePCOpen + slewRate), inputState.maxPCOpen)); // Capped at maximum.
      }
    // Keep open at maximum allowed.
    return(inputState.maxPCOpen);
    }

  // (Well) over temp target: close valve down.
  if(adjustedTempC > inputState.targetTempC)
    {
//DEBUG_SERIAL_PRINTLN_FLASHSTRING("over temp");

    if(0 != valvePCOpen)
      {
      // Reduce valve hunting: defer re-closing if recently opened.
      if(retainedState.dontTurndown()) { return(valvePCOpen); }

      // True if just above the the proportional range.
      const bool justOverTemp = (adjustedTempC == inputState.targetTempC+1);

      // TODO-453: avoid closing the valve at all when the temperature error is small and falling, and there is a widened deadband.
      if(justOverTemp && inputState.widenDeadband && (retainedState.getRawDelta() < 0)) { return(valvePCOpen); }

      // TODO-482: glacial close if temperature is jittery and not too far above target.
      if(justOverTemp && retainedState.isFiltering) { return(valvePCOpen - 1); }

      // Continue shutting valve slowly as not yet fully closed.
      // TODO-117: allow very slow final turn off to help systems with poor bypass, ~1% per minute.
      // Special slow-turn-off rules for final part of travel at/below "min % really open" floor.
      const uint8_t minReallyOpen = inputState.minPCOpen;
      const uint8_t lingerThreshold = (minReallyOpen > 0) ? (minReallyOpen-1) : 0;
      if(valvePCOpen < minReallyOpen)
        {
        // If lingered long enough then do final chunk in one burst to help avoid valve hiss and temperature overshoot.
        if((DEFAULT_MAX_RUN_ON_TIME_M < minReallyOpen) && (valvePCOpen < minReallyOpen - DEFAULT_MAX_RUN_ON_TIME_M))
          { return(0); } // Shut valve completely.
        return(valvePCOpen - 1); // Turn down as slowly as reasonably possible to help boiler cool.
        }

      // TODO-109: with comfort bias close relatively slowly to reduce wasted effort from minor overshoots.
      // TODO-453: close relatively slowly when temperature error is small (<1C) to reduce wasted effort from minor overshoots.
      if(((!inputState.hasEcoBias) || justOverTemp || retainedState.isFiltering) &&
         (valvePCOpen > constrain(((int)lingerThreshold) + TRV_SLEW_PC_PER_MIN_FAST, TRV_SLEW_PC_PER_MIN_FAST, inputState.maxPCOpen)))
        { return(valvePCOpen - TRV_SLEW_PC_PER_MIN_FAST); }

      // Else (by default) force to (nearly) off immediately when requested, ie eagerly stop heating to conserve energy.
      // In any case percentage open should now be low enough to stop calling for heat immediately.
      return(lingerThreshold);
      }

    // Ensure that the valve is/remains fully shut.
    return(0);
    }

  // Close to (or at) temp target: set valve partly open to try to tightly regulate.
  //
  // Use currentTempC16 lsbits to set valve percentage for proportional feedback
  // to provide more efficient and quieter TRV drive and probably more stable room temperature.
  const uint8_t lsbits = (uint8_t) (adjustedTempC16 & 0xf); // LSbits of temperature above base of proportional adjustment range.
//    uint8_t tmp = (uint8_t) (refTempC16 & 0xf); // Only interested in lsbits.
  const uint8_t tmp = 16 - lsbits; // Now in range 1 (at warmest end of 'correct' temperature) to 16 (coolest).
  const uint8_t ulpStep = 6;
  // Get to nominal range 6 to 96, eg valve nearly shut just below top of 'correct' temperature window.
  const uint8_t targetPORaw = tmp * ulpStep;
  // Constrain from below to likely minimum-open value, in part to deal with TODO-117 'linger open' in lieu of boiler bypass.
  // Constrain from above by maximum percentage open allowed, eg for pay-by-volume systems.
  const uint8_t targetPO = constrain(targetPORaw, inputState.minPCOpen, inputState.maxPCOpen);

  // Reduce spurious valve/boiler adjustment by avoiding movement at all unless current temperature error is significant.
  if(targetPO != valvePCOpen)
    {
    // True iff valve needs to be closed somewhat.
    const bool tooOpen = (targetPO < valvePCOpen);
    // Compute the minimum/epsilon slew adjustment allowed (the deadband).
    // Also increase effective deadband if temperature resolution is lower than 1/16th, eg 8ths => 1+2*ulpStep minimum.
// FIXME //    const uint8_t realMinUlp = 1 + (inputState.isLowPrecision ? 2*ulpStep : ulpStep); // Assume precision no coarser than 1/8C.
    const uint8_t realMinUlp = 1 + ulpStep;
    const uint8_t minAbsSlew = fnmax(realMinUlp, (uint8_t)(inputState.widenDeadband ? max(min(DEFAULT_VALVE_PC_MODERATELY_OPEN/2,max(TRV_MAX_SLEW_PC_PER_MIN,2*TRV_MIN_SLEW_PC)), 2+TRV_MIN_SLEW_PC) : TRV_MIN_SLEW_PC));
    if(tooOpen) // Currently open more than required.  Still below target at top of proportional range.
      {
//DEBUG_SERIAL_PRINTLN_FLASHSTRING("slightly too open");
      const uint8_t slew = valvePCOpen - targetPO;
      // Ensure no hunting for ~1ulp temperature wobble.
      if(slew < minAbsSlew) { return(valvePCOpen); }

      // Reduce valve hunting: defer re-closing if recently opened.
      if(retainedState.dontTurndown()) { return(valvePCOpen); }

      // TODO-453: avoid closing the valve at all when the (raw) temperature is not rising, so as to minimise valve movement.
      // Since the target is the top of the proportional range than nothing within it requires the temperature to be *forced* down.
      // Possibly don't apply this rule at the very top of the range in case filtering is on and the filtered value moves differently to the raw.
      if(retainedState.getRawDelta() <= 0) { return(valvePCOpen); }

      // Close glacially if explicitly requested or if temperature undershoot has happened or is a danger.
      // Also be glacial if in soft setback which aims to allow temperatures to drift passively down a little.
      //   (TODO-451, TODO-467: have darkness only immediately trigger a 'soft setback' using wide deadband)
      // This assumes that most valves more than about 1/3rd open can deliver significant power, esp if not statically balanced.
      // TODO-482: try to deal better with jittery temperature readings.
      const bool beGlacial = inputState.glacial ||
#if defined(GLACIAL_ON_WITH_WIDE_DEADBAND)
          ((inputState.widenDeadband || retainedState.isFiltering) && (valvePCOpen <= DEFAULT_VALVE_PC_MODERATELY_OPEN)) ||
#endif
          (lsbits < 8);
      if(beGlacial) { return(valvePCOpen - 1); }

      if(slew > TRV_SLEW_PC_PER_MIN_FAST)
          { return(valvePCOpen - TRV_SLEW_PC_PER_MIN_FAST); } // Cap slew rate.
      // Adjust directly to target.
      return(targetPO);
      }

    // if(targetPO > TRVPercentOpen) // Currently open less than required.  Still below target at top of proportional range.
//DEBUG_SERIAL_PRINTLN_FLASHSTRING("slightly too closed");
#if defined(SUPPORT_BAKE)
    // If room is well below target and in BAKE mode then immediately open to maximum.
    // Needs debounced bake mode value to avoid spuriously slamming open the valve as the user cycles through modes.
    if(inputState.inBakeMode) { return(inputState.maxPCOpen); }
#endif

    const uint8_t slew = targetPO - valvePCOpen;
    // To to avoid hunting around boundaries of a ~1ulp temperature step.
    if(slew < minAbsSlew) { return(valvePCOpen); }

    // Reduce valve hunting: defer re-opening if recently closed.
    if(retainedState.dontTurnup()) { return(valvePCOpen); }

    // TODO-453: minimise valve movement (and thus noise and battery use).
    // Keeping the temperature steady anywhere in the target proportional range
    // while minimising valve moment/noise/etc is a good goal,
    // so if raw temperatures are rising at the moment then leave the valve as-is.
    // If fairly near the final target then also leave the valve as-is (TODO-453 & TODO-451).
    const int rise = retainedState.getRawDelta();
    if(rise > 0) { return(valvePCOpen); }
    if( /* (0 == rise) && */ (lsbits >= (inputState.widenDeadband ? 8 : 12))) { return(valvePCOpen); }

    // Open glacially if explicitly requested or if temperature overshoot has happened or is a danger.
    // Also be glacial if in soft setback which aims to allow temperatures to drift passively down a little.
    //   (TODO-451, TODO-467: have darkness only immediately trigger a 'soft setback' using wide deadband)
    // This assumes that most valves more than about 1/3rd open can deliver significant power, esp if not statically balanced.
    const bool beGlacial = inputState.glacial ||
#if defined(GLACIAL_ON_WITH_WIDE_DEADBAND)
        inputState.widenDeadband ||
#endif
        (lsbits >= 8) || ((lsbits >= 4) && (valvePCOpen >= DEFAULT_VALVE_PC_MODERATELY_OPEN));
    if(beGlacial) { return(valvePCOpen + 1); }

    // Slew open faster with comfort bias.
    const uint8_t maxSlew = (!inputState.hasEcoBias) ? TRV_SLEW_PC_PER_MIN_FAST : TRV_MAX_SLEW_PC_PER_MIN;
    if(slew > maxSlew)
        { return(valvePCOpen + maxSlew); } // Cap slew rate open.
    // Adjust directly to target.
    return(targetPO);
    }

  // Leave value position as was...
  return(valvePCOpen);
  }

// Compute/update target temperature and set up state for tick()/computeRequiredTRVPercentOpen().
void ModelledRadValve::computeTargetTemperature()
  {
  // Compute basic target temperature.
  const uint8_t newTarget = computeTargetTemp();

  // Set up state for computeRequiredTRVPercentOpen().
  inputState.targetTempC = newTarget;
  inputState.minPCOpen = getMinPercentOpen();
  inputState.maxPCOpen = getMaxPercentageOpenAllowed();
  inputState.glacial = glacial;
  inputState.inBakeMode = inBakeMode();
  inputState.hasEcoBias = hasEcoBias();
  // Widen the allowed deadband significantly in a dark/quiet/vacant room (TODO-383)
  // (or in FROST mode, or if temperature is jittery eg changing fast and filtering has been engaged)
  // to attempt to reduce the total number and size of adjustments and thus reduce noise/disturbance (and battery drain).
  // The wider deadband (less good temperature regulation) might be noticeable/annoying to sensitive occupants.
  // FIXME: With a wider deadband may also simply suppress any movement/noise on some/most minutes while close to target temperature.
  inputState.widenDeadband = AmbLight.isRoomDark() || Occupancy.longVacant() || (!inWarmMode()) || retainedState.isFiltering;
  // Capture adjusted reference/room temperatures
  // and set callingForHeat flag also using same outline logic as computeRequiredTRVPercentOpen() will use.
  inputState.setReferenceTemperatures(TemperatureC16.get());
  callingForHeat = (newTarget >= (inputState.refTempC16 >> 4));
  }

// Get smoothed raw/unadjusted temperature from the most recent samples.
int ModelledRadValveState::getSmoothedRecent()
  { return(smallIntMean<filterLength>(prevRawTempC16)); }

//// Compute an estimate of rate/velocity of temperature change in C/16 per minute/tick.
//// A positive value indicates that temperature is rising.
//// Based on comparing the most recent smoothed value with an older smoothed value.
//int ModelledRadValveState::getVelocityC16PerTick()
//  {
//  const int oldSmoothed = smallIntMean<filterLength/2>(prevRawTempC16 + (filterLength/2));
//  const int newSmoothed = getSmoothedRecent();
//  const int velocity = (newSmoothed - oldSmoothed + (int)(filterLength/4)) / (int)(filterLength/2); // Avoid going unsigned by accident.
////DEBUG_SERIAL_PRINT_FLASHSTRING("old&new sm, velocity: ");
////DEBUG_SERIAL_PRINT(oldSmoothed);
////DEBUG_SERIAL_PRINT('&');
////DEBUG_SERIAL_PRINT(newSmoothed);
////DEBUG_SERIAL_PRINT(',');
////DEBUG_SERIAL_PRINT(velocity);
////DEBUG_SERIAL_PRINTLN();
//  return(velocity);
//  }

// Maximum jump between adjacent readings before forcing filtering.
// Too small a value may in some circumstances cap room rate rise to this per minute.
// Too large a value may fail to sufficiently help damp oscillations and overshoot.
// As to be at least as large as the minimum temperature sensor precision to avoid false triggering of the filter.
// Typical values range from 2 (for 1/8C precision temperature sensor) up to 4.
static const int MAX_TEMP_JUMP_C16 = 3; // 3/16C.

// Perform per-minute tasks such as counter and filter updates then recompute valve position.
// The input state must be complete including target and reference temperatures
// before calling this including the first time whereupon some further lazy initialisation is done.
//   * valvePCOpenRef  current valve position UPDATED BY THIS ROUTINE, in range [0,100]
void ModelledRadValveState::tick(volatile uint8_t &valvePCOpenRef, const ModelledRadValveInputState &inputState)
  {
  const int rawTempC16 = inputState.refTempC16 - refTempOffsetC16; // Remove adjustment for target centre.
  if(!initialised)
    {
    // Fill the filter memory with the current room temperature.
    for(int i = filterLength; --i >= 0; ) { prevRawTempC16[i] = rawTempC16; }
    initialised = true;
    }

  // Shift in the latest (raw) temperature.
  for(int i = filterLength; --i > 0; ) { prevRawTempC16[i] = prevRawTempC16[i-1]; }
  prevRawTempC16[0] = rawTempC16;

  // Disable/enable filtering.
  // Allow possible exit from filtering for next time
  // if the raw value is close enough to the current filtered value
  // so that reverting to unfiltered will not of itself cause a big jump.
  if(isFiltering)
    {
    if(abs(getSmoothedRecent() - rawTempC16) <= MAX_TEMP_JUMP_C16) { isFiltering = false; }  
    }
  // Force filtering (back) on if any adjacent past readings are wildly different.
  if(!isFiltering)
    {
    for(int i = 1; i < filterLength; ++i) { if(abs(prevRawTempC16[i] - prevRawTempC16[i-1]) > MAX_TEMP_JUMP_C16) { isFiltering = true; break; } }
    }

  // Tick count down timers.
  if(valveTurndownCountdownM > 0) { --valveTurndownCountdownM; }
  if(valveTurnupCountdownM > 0) { --valveTurnupCountdownM; }

  // Update the modelled state including the valve position passed by reference.
  const uint8_t newValvePC = ModelledRadValve::computeRequiredTRVPercentOpen(valvePCOpenRef, inputState, *this);
  const bool changed = (newValvePC != valvePCOpenRef);
  if(changed)
    {
    if(newValvePC > valvePCOpenRef)
      {
      // Defer reclosing valve to avoid excessive hunting.
      valveTurnup();
      cumulativeMovementPC += (newValvePC - valvePCOpenRef);
      }
    else
      {
      // Defer opening valve to avoid excessive hunting.
      valveTurndown();
      cumulativeMovementPC += (valvePCOpenRef - newValvePC);
      }
    valvePCOpenRef = newValvePC;
    }
  valveMoved = changed;
  }

// Compute target temperature and set heat demand for TRV and boiler; update state.
// CALL REGULARLY APPROXIMATELY ONCE PER MINUTE TO ALLOW SIMPLE TIME-BASED CONTROLS.
// Inputs are inWarmMode(), isRoomLit().
// The inputs must be valid (and recent).
// Values set are targetTempC, value (TRVPercentOpen).
// This may also prepare data such as TX command sequences for the TRV, boiler, etc.
// This routine may take significant CPU time; no I/O is done, only internal state is updated.
// Returns true if valve target changed and thus messages may need to be recomputed/sent/etc.
void ModelledRadValve::computeCallForHeat()
  {
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
#ifdef SUPPORT_BAKE
    // Cancel any BAKE mode once temperature target has been hit.
    if(!callingForHeat) { bakeCountdownM = 0; }
    // Run down BAKE mode timer if need be, one tick per minute.
    else if(bakeCountdownM > 0) { --bakeCountdownM; }
#endif
    }

  // Compute target and ensure that required input state is set for computeRequiredTRVPercentOpen().
  computeTargetTemperature();
  retainedState.tick(value, inputState);
  }




// The STATS_SMOOTH_SHIFT is chosen to retain some reasonable precision within a byte and smooth over a weekly cycle.
#define STATS_SMOOTH_SHIFT 3 // Number of bits of shift for smoothed value: larger => larger time-constant; strictly positive.

// If defined, limit to stats sampling to one pre-sample and the final sample, to simplify/speed code.
#define STATS_MAX_2_SAMPLES

// Compute new linearly-smoothed value given old smoothed value and new value.
// Guaranteed not to produce a value higher than the max of the old smoothed value and the new value.
// Uses stochastic rounding to nearest to allow nominally sub-lsb values to have an effect over time.
// Usually only made public for unit testing.
uint8_t smoothStatsValue(const uint8_t oldSmoothed, const uint8_t newValue)
  {
  if(oldSmoothed == newValue) { return(oldSmoothed); } // Optimisation: smoothed value is unchanged if new value is the same as extant.
  // Compute and update with new stochastically-rounded exponentially-smoothed ("Brown's simple exponential smoothing") value.
  // Stochastic rounding allows sub-lsb values to have an effect over time.
  const uint8_t stocAdd = randRNG8() & ((1 << STATS_SMOOTH_SHIFT) - 1); // Allows sub-lsb values to have an effect over time.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("stocAdd=");
  DEBUG_SERIAL_PRINT(stocAdd);
  DEBUG_SERIAL_PRINTLN();
#endif
  // Do arithmetic in 16 bits to avoid over-/under- flows.
  return((uint8_t) (((((uint16_t) oldSmoothed) << STATS_SMOOTH_SHIFT) - ((uint16_t)oldSmoothed) + ((uint16_t)newValue) + stocAdd) >> STATS_SMOOTH_SHIFT));
  }

// Do an efficient division of an int total by small positive count to give a uint8_t mean.
//  * total running total, no higher than 255*sampleCount
//  * sampleCount small (<128) strictly positive number
static uint8_t smartDivToU8(const uint16_t total, const uint8_t sampleCount)
  {
#if 0 && defined(DEBUG) // Extra arg validation during dev.
  if(0 == sampleCount) { panic(); }
#endif
  if(1 == sampleCount) { return((uint8_t) total); } // No division required.
#if !defined(STATS_MAX_2_SAMPLES)
  // Generic divide (slow).
  if(2 != sampleCount) { return((uint8_t) ((total + (sampleCount>>1)) / sampleCount)); }
#elif 0 && defined(DEBUG)
  if(2 != sampleCount) { panic(); }
#endif
  // 2 samples.
  return((uint8_t) ((total+1) >> 1)); // Fast shift for 2 samples instead of slow divide.
  }

// Do simple update of last and smoothed stats numeric values.
// This assumes that the 'last' set is followed by the smoothed set.
// This autodetects unset values in the smoothed set and replaces them completely.
//   * lastSetPtr  is the offset in EEPROM of the 'last' value, with 'smoothed' assumed to be 24 bytes later.
//   * value  new stats value in range [0,254]
static void simpleUpdateStatsPair_(uint8_t * const lastEEPtr, const uint8_t value)
  {
#if 0 && defined(DEBUG) // Extra arg validation during dev.
  if((((int)lastEEPtr) < EE_START_STATS) || (((int)lastEEPtr)+24 > EE_END_STATS)) { panic(); }
  if(0xff == value) { panic(); }
#endif
  // Update the last-sample slot using the mean samples value.
  eeprom_smart_update_byte(lastEEPtr, value);
  // If existing smoothed value unset or invalid, use new one as is, else fold in.
  uint8_t * const pS = lastEEPtr + 24;
  const uint8_t smoothed = eeprom_read_byte(pS);
  if(0xff == smoothed) { eeprom_smart_update_byte(pS, value); }
  else { eeprom_smart_update_byte(pS, smoothStatsValue(smoothed, value)); }
  }
// Get some constant calculation done at compile time,
//   * lastSetN  is the set number for the 'last' values, with 'smoothed' assumed to be the next set.
//   * hh  hour for these stats [0,23].
//   * value  new stats value in range [0,254].
static inline void simpleUpdateStatsPair(const uint8_t lastSetN, const uint8_t hh, const uint8_t value)
  {
#if 0 && defined(DEBUG)
    DEBUG_SERIAL_PRINT_FLASHSTRING("stats update for set ");
    DEBUG_SERIAL_PRINT(lastSetN);
    DEBUG_SERIAL_PRINT_FLASHSTRING(" @");
    DEBUG_SERIAL_PRINT(hh);
    DEBUG_SERIAL_PRINT_FLASHSTRING("h = ");
    DEBUG_SERIAL_PRINT(value);
    DEBUG_SERIAL_PRINTLN();
#endif
  simpleUpdateStatsPair_((uint8_t *)(EE_STATS_START_ADDR(lastSetN) + (hh)), (value));
  }

// Sample statistics once per hour as background to simple monitoring and adaptive behaviour.
// Call this once per hour with fullSample==true, as near the end of the hour as possible;
// this will update the non-volatile stats record for the current hour.
// Optionally call this at a small (2--10) even number of evenly-spaced number of other times thoughout the hour
// with fullSample=false to sub-sample (and these may receive lower weighting or be ignored).
// (EEPROM wear should not be an issue at this update rate in normal use.)
void sampleStats(const bool fullSample)
  {
  // (Sub-)sample processing.
  // In general, keep running total of sub-samples in a way that should not overflow
  // and use the mean to update the non-volatile EEPROM values on the fullSample call.
  static uint8_t sampleCount_; // General sub-sample count; initially zero after boot, and zeroed after each full sample.
#if defined(STATS_MAX_2_SAMPLES)
  // Ensure maximum of two samples used: optional non-full sample then full/final one.
  if(!fullSample && (sampleCount_ != 0)) { return; }
#endif
  const bool firstSample = (0 == sampleCount_++);
#if defined(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK)
  // WARM mode count.
  static int8_t warmCount; // Sub-sample WARM count; initially zero, and zeroed after each full sample.
  if(inWarmMode()) { ++warmCount; } else { --warmCount; }
#endif
  // Ambient light.
  const uint16_t ambLight = fnmin(AmbLight.get(), (uint8_t)MAX_STATS_AMBLIGHT); // Constrain value at top end to avoid 'not set' value.
  static uint16_t ambLightTotal;
  ambLightTotal = firstSample ? ambLight : (ambLightTotal + ambLight);
  const int tempC16 = TemperatureC16.get();
  static int tempC16Total;
  tempC16Total = firstSample ? tempC16 : (tempC16Total + tempC16);
#ifdef OCCUPANCY_SUPPORT
  const uint16_t occpc = Occupancy.get();
  static uint16_t occpcTotal;
  occpcTotal = firstSample ? occpc : (occpcTotal + occpc);
#endif
#if defined(HUMIDITY_SENSOR_SUPPORT)
  // Assume for now RH% always available (compile-time determined) or not; not intermittent.
  // TODO: allow this to work with at least start-up-time availability detection.
  const uint16_t rhpc = fnmin(RelHumidity.get(), (uint8_t)100); // Fail safe.
  static uint16_t rhpcTotal;
  rhpcTotal = firstSample ? rhpc : (rhpcTotal + rhpc);
#endif
  if(!fullSample) { return; } // Only accumulate values cached until a full sample.
  // Catpure sample count to use below.
  const uint8_t sc = sampleCount_; 
  // Reset generic sub-sample count to initial state after fill sample.
  sampleCount_ = 0;

  // Get the current local-time hour...
  const uint_least8_t hh = getHoursLT(); 

  // Scale and constrain last-read temperature to valid range for stats.
#if defined(STATS_MAX_2_SAMPLES)
  const int tempCTotal = (1==sc)?tempC16Total:((tempC16Total+1)>>1);
#else
  const int tempCTotal = (1==sc)?tempC16Total:
                         ((2==sc)?((tempC16Total+1)>>1):
                                  ((tempC16Total + (sc>>1)) / sc));
#endif
  const uint8_t temp = compressTempC16(tempCTotal);
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("SU tempC16Total=");
  DEBUG_SERIAL_PRINT(tempC16Total);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", tempCTotal=");
  DEBUG_SERIAL_PRINT(tempCTotal);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", temp=");
  DEBUG_SERIAL_PRINT(temp);
  DEBUG_SERIAL_PRINT_FLASHSTRING(", expanded=");
  DEBUG_SERIAL_PRINT(expandTempC16(temp));
  DEBUG_SERIAL_PRINTLN();
#endif
  simpleUpdateStatsPair(EE_STATS_SET_TEMP_BY_HOUR, hh, temp);

  // Ambient light; last and smoothed data sets,
  simpleUpdateStatsPair(EE_STATS_SET_AMBLIGHT_BY_HOUR, hh, smartDivToU8(ambLightTotal, sc));

#ifdef OCCUPANCY_SUPPORT
  // Occupancy confidence percent, if supported; last and smoothed data sets,
  simpleUpdateStatsPair(EE_STATS_SET_OCCPC_BY_HOUR, hh, smartDivToU8(occpcTotal, sc));
#endif 

#if defined(HUMIDITY_SENSOR_SUPPORT)
  // Relative humidity percent, if supported; last and smoothed data sets,
  simpleUpdateStatsPair(EE_STATS_SET_RHPC_BY_HOUR, hh, smartDivToU8(rhpcTotal, sc));
#endif

#if defined(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK)
  // Update sampled WARM-mode value.
  // 0xff when unset/erased; first use will set all history bits to the initial sample value.
  // When in use, bit 7 (msb) is always 0 (to distinguish from unset).
  // Bit 6 is 1 if most recent day's sample was in WARM (or BAKE) mode, 0 if in FROST mode.
  // At each new sampling, bits 6--1 are shifted down and the new bit 6 set as above.
  // Designed to enable low-wear no-write or selective erase/write use much of the time;
  // periods which are always the same mode will achieve a steady-state value (eliminating most EEPROM wear)
  // while even some of the rest (while switching over from all-WARM to all-FROST) will only need pure writes (no erase).
  uint8_t *const phW = (uint8_t *)(EE_STATS_START_ADDR(EE_STATS_SET_WARMMODE_BY_HOUR_OF_WK) + hh);
  const uint8_t warmHistory = eeprom_read_byte(phW);
  if(warmHistory & 0x80) { eeprom_smart_clear_bits(phW, inWarmMode() ? 0x7f : 0); } // First use sets all history bits to current sample value.
  else // Shift in today's sample bit value for this hour at bit 6...
    {
    uint8_t newWarmHistory = (warmHistory >> 1) & 0x3f;
    if(warmCount > 0) { newWarmHistory |= 0x40; } // Treat as warm iff more WARM than FROST (sub-)samples.
    eeprom_smart_update_byte(phW, newWarmHistory);
    }
  // Reset WARM sub-sample count after full sample.
  warmCount = 0;
#endif

  // TODO: other stats measures...
  }

// Get raw stats value for hour HH [0,23] from stats set N from non-volatile (EEPROM) store.
// A value of 0xff (255) means unset (or out of range); other values depend on which stats set is being used.
// The stats set is determined by the order in memory.
uint8_t getByHourStat(uint8_t hh, uint8_t statsSet)
  {
  if(statsSet > (EE_END_STATS - EE_START_STATS) / EE_STATS_SET_SIZE) { return((uint8_t) 0xff); } // Invalid set.
  if(hh > 23) { return((uint8_t) 0xff); } // Invalid hour.
  return(eeprom_read_byte((uint8_t *)(EE_START_STATS + (statsSet * (int)EE_STATS_SET_SIZE) + (int)hh)));
  }


// Clear all collected statistics, eg when moving device to a new room or at a major time change.
// Requires 1.8ms per byte for each byte that actually needs erasing.
//   * maxBytesToErase limit the number of bytes erased to this; strictly positive, else 0 to allow 65536
// Returns true if finished with all bytes erased.
bool zapStats(uint16_t maxBytesToErase)
  {
  for(uint8_t *p = (uint8_t *)EE_START_STATS; p <= (uint8_t *)EE_END_STATS; ++p)
    { if(eeprom_smart_erase_byte(p)) { if(--maxBytesToErase == 0) { return(false); } } } // Stop if out of time...
  return(true); // All done.
  }


// Range-compress an signed int 16ths-Celsius temperature to a unsigned single-byte value < 0xff.
// This preserves at least the first bit after the binary point for all values,
// and three bits after binary point for values in the most interesting mid range around normal room temperatures,
// with transitions at whole degrees Celsius.
// Input values below 0C are treated as 0C, and above 100C as 100C, thus allowing air and DHW temperature values.
#define COMPRESSION_C16_FLOOR_VAL 0 // Floor input value to compression.
#define COMPRESSION_C16_LOW_THRESHOLD (16<<4) // Values in range [COMPRESSION_LOW_THRESHOLD_C16,COMPRESSION_HIGH_THRESHOLD_C16[ have maximum precision.
#define COMPRESSION_C16_LOW_THR_AFTER (COMPRESSION_C16_LOW_THRESHOLD>>3) // Low threshold after compression.
#define COMPRESSION_C16_HIGH_THRESHOLD (24<<4)
#define COMPRESSION_C16_HIGH_THR_AFTER (COMPRESSION_C16_LOW_THR_AFTER + ((COMPRESSION_C16_HIGH_THRESHOLD-COMPRESSION_C16_LOW_THRESHOLD)>>1)) // High threshold after compression.
#define COMPRESSION_C16_CEIL_VAL (100<<4) // Ceiling input value to compression.
#define COMPRESSION_C16_CEIL_VAL_AFTER (COMPRESSION_C16_HIGH_THR_AFTER + ((COMPRESSION_C16_CEIL_VAL-COMPRESSION_C16_HIGH_THRESHOLD) >> 3)) // Ceiling input value after compression.
uint8_t compressTempC16(int tempC16)
  {
  if(tempC16 <= 0) { return(0); } // Clamp negative values to zero.
  if(tempC16 < COMPRESSION_C16_LOW_THRESHOLD) { return(tempC16 >> 3); } // Preserve 1 bit after the binary point (0.5C precision).
  if(tempC16 < COMPRESSION_C16_HIGH_THRESHOLD)
    { return(((tempC16 - COMPRESSION_C16_LOW_THRESHOLD) >> 1) + COMPRESSION_C16_LOW_THR_AFTER); }
  if(tempC16 < COMPRESSION_C16_CEIL_VAL)
    { return(((tempC16 - COMPRESSION_C16_HIGH_THRESHOLD) >> 3) + COMPRESSION_C16_HIGH_THR_AFTER); }
  return(COMPRESSION_C16_CEIL_VAL_AFTER);
  }

// Reverses range compression done by compressTempC16(); results in range [0,100], with varying precision based on original value.
// 0xff (or other invalid) input results in STATS_UNSET_INT. 
int expandTempC16(uint8_t cTemp)
  {
  if(cTemp < COMPRESSION_C16_LOW_THR_AFTER) { return(cTemp << 3); }
  if(cTemp < COMPRESSION_C16_HIGH_THR_AFTER)
    { return(((cTemp - COMPRESSION_C16_LOW_THR_AFTER) << 1) + COMPRESSION_C16_LOW_THRESHOLD); }
  if(cTemp <= COMPRESSION_C16_CEIL_VAL_AFTER)
    { return(((cTemp - COMPRESSION_C16_HIGH_THR_AFTER) << 3) + COMPRESSION_C16_HIGH_THRESHOLD); }
  return(STATS_UNSET_INT); // Invalid/unset input.
  }


#ifdef ENABLE_ANTICIPATION
// Returns true if system is in 'learn'/smart mode.
// If in 'smart' mode then the unit can anticipate user demand
// to pre-warm rooms, maintain customary temperatures, etc.
// Currently true if any simple schedule is set.
bool inSmartMode()
  {
  return(isAnySimpleScheduleSet());
  }
#endif











// Clear and populate core stats structure with information from this node.
// Exactly what gets filled in will depend on sensors on the node,
// and may depend on stats TX security level (eg if collecting some sensitive items is also expensive).
void populateCoreStats(FullStatsMessageCore_t *const content)
  {
  clearFullStatsMessageCore(content); // Defensive programming: all fields should be set explicitly below.
  if(localFHT8VTRVEnabled())
    {
    // Use FHT8V house codes if available.
    content->id0 = FHT8VGetHC1();
    content->id1 = FHT8VGetHC2();
    }
  else
    {
    // Use OpenTRV unique ID if no other higher-priority ID.
    content->id0 = eeprom_read_byte(0 + (uint8_t *)EE_START_ID);
    content->id1 = eeprom_read_byte(1 + (uint8_t *)EE_START_ID);
    }
  content->containsID = true;
  content->tempAndPower.tempC16 = TemperatureC16.get();
  content->tempAndPower.powerLow = Supply_mV.isSupplyVoltageLow();
  content->containsTempAndPower = true;
  content->ambL = fnmax((uint8_t)1, fnmin((uint8_t)254, (uint8_t)(AmbLight.get() >> 2))); // Coerce to allowed value in range [1,254]. Bug-fix c/o Gary Gladman!
  content->containsAmbL = true;
  // OC1/OC2 = Occupancy: 00 not disclosed, 01 not occupied, 10 possibly occupied, 11 probably occupied.
  // The encodeFullStatsMessageCore() route should omit data not appopriate for security reasons.
  content->occ = Occupancy.twoBitOccupancyValue();
  }








// Call this to do an I/O poll if needed; returns true if something useful happened.
// This call should typically take << 1ms at 1MHz CPU.
// Does not change CPU clock speeds, mess with interrupts (other than possible brief blocking), or sleep.
// Limits actual poll rate to something like once every 32ms, unless force is true.
//   * force if true then force full poll on every call (ie do not internally rate-limit)
bool pollIO(const bool force)
  {
#if defined(ENABLE_BOILER_HUB) && defined(USE_MODULE_FHT8VSIMPLE)
  if(inHubMode())
    {
    static volatile uint8_t _pO_lastPoll;

    // Poll RX at most about every ~32ms to help approx match spil rate when called in loop with 30ms nap.
    const uint8_t sct = getSubCycleTime();
    if(force || ((0 == (sct & 3)) && (sct != _pO_lastPoll)))
      {
      _pO_lastPoll = sct;
      if(FHT8VCallForHeatPoll()) // Check if call-for-heat has been overheard.
        { return(true); }
      }
    }
#endif
  return(false);
  }


#if defined(ALLOW_JSON_OUTPUT)
// Managed JSON stats.
static SimpleStatsRotation<8> ss1; // Configured for maximum different stats.
#endif

// Do bare stats transmission.
// Output should be filtered for items appropriate
// to current channel security and sensitivity level.
// This may be binary or JSON format.
//   * resumeRX  if true and unit capable of running in hub/RX mode,
//       the unit will resume RX after sending the stats
//   * allowDoubleTX  allow double TX to increase chance of successful reception
//   * doBinary  send binary form, else JSON form if supported
static void bareStatsTX(const bool resumeRX, const bool allowDoubleTX, const bool doBinary)
  {
#if (FullStatsMessageCore_MAX_BYTES_ON_WIRE > STATS_MSG_MAX_LEN)
#error FullStatsMessageCore_MAX_BYTES_ON_WIRE too big
#endif
#if (MSG_JSON_MAX_LENGTH+1 > STATS_MSG_MAX_LEN) // Allow 1 for trailing CRC.
#error MSG_JSON_MAX_LENGTH too big
#endif
  
  // Allow space in buffer for:
  //   * buffer offset/preamble
  //   * max binary length, or max JSON length + 1 for CRC + 1 to allow detection of oversize message
  //   * terminating 0xff
  uint8_t buf[STATS_MSG_START_OFFSET + max(FullStatsMessageCore_MAX_BYTES_ON_WIRE,  MSG_JSON_MAX_LENGTH+1) + 1];

#if defined(ALLOW_JSON_OUTPUT)
  if(doBinary)
#endif
    {
    // Send binary message first.
    // Gather core stats.
    FullStatsMessageCore_t content;
    populateCoreStats(&content);
    const uint8_t *msg1 = encodeFullStatsMessageCore(buf + STATS_MSG_START_OFFSET, sizeof(buf) - STATS_MSG_START_OFFSET, getStatsTXLevel(), false, &content);
    if(NULL == msg1)
      {
#if 0
DEBUG_SERIAL_PRINTLN_FLASHSTRING("Bin gen err!");
#endif
      return;
      }
    // Record stats as if remote, and treat channel as secure.
    recordCoreStats(true, &content);
    // Send it!
    RFM22RawStatsTX(true, buf, resumeRX, allowDoubleTX);
    }

#if defined(ALLOW_JSON_OUTPUT)
  else // Send binary or JSON on each attempt so as not to overwhelm the receiver.
    {
    // Send JSON message.        
    uint8_t *bptr = buf + STATS_MSG_START_OFFSET;
    // Now append JSON text and closing 0xff...
    // Use letters that correspond to the values in ParsedRemoteStatsRecord and when displaying/parsing @ status records.
    int8_t wrote;

    // Managed JSON stats.
//    static SimpleStatsRotation<8> ss1; // Configured for maximum different stats.
    const bool maximise = true; // Make best use of available bandwidth...
    if(ss1.isEmpty())
      {
#ifdef DEBUG
      ss1.enableCount(true); // For diagnostic purposes, eg while TX is lossy.
#endif
//      // Try and get as much out on the first TX as possible.
//      maximise = true;
      }
    ss1.put(TemperatureC16);
#if defined(HUMIDITY_SENSOR_SUPPORT)
    ss1.put(RelHumidity);
#endif
#if defined(OCCUPANCY_SUPPORT)
    ss1.put(Occupancy.twoBitTag(), Occupancy.twoBitOccupancyValue()); // Reduce spurious TX cf percentage.
    ss1.put(Occupancy.vacHTag(), Occupancy.getVacancyH()); // EXPERIMENTAL
#endif
    // OPTIONAL items
    // Only TX supply voltage for units apparently not mains powered.
    if(!Supply_mV.isMains()) { ss1.put(Supply_mV); } else { ss1.remove(Supply_mV.tag()); }
#if !defined(LOCAL_TRV) // Deploying as sensor unit, not TRV controller, so show all sensors and no TRV stuff.
    // Only show ambient light levels for non-TRV pure-sensor units.
    ss1.put(AmbLight);
#else
    ss1.put(NominalRadValve);
    ss1.put(NominalRadValve.tagTTC(), NominalRadValve.getTargetTempC());
#if 1
    ss1.put(NominalRadValve.tagCMPC(), NominalRadValve.getCumulativeMovementPC()); // EXPERIMENTAL
#endif
#endif
    // If not doing a doubleTX then consider sometimes suppressing the change-flag clearing for this send
    // to reduce the chance of important changes being missed by the receiver.
    wrote = ss1.writeJSON(bptr, sizeof(buf) - (bptr-buf), getStatsTXLevel(), maximise); // , !allowDoubleTX && randRNG8NextBoolean());
    if(0 == wrote)
      {
DEBUG_SERIAL_PRINTLN_FLASHSTRING("JSON gen err!");
      return;
      }

    // Record stats as if local, and treat channel as secure.
    recordJSONStats(true, (const char *)bptr);
#if 0 || !defined(ENABLE_BOILER_HUB) && defined(DEBUG)
    DEBUG_SERIAL_PRINT((const char *)bptr);
    DEBUG_SERIAL_PRINTLN();
#endif
    // Adjust JSON message for reliable transmission.
    // (Set high-bit on final '}' to make it unique, and compute and append (non-0xff) CRC.)
    const uint8_t crc = adjustJSONMsgForTXAndComputeCRC((char *)bptr);
    if(0xff == crc)
      {
  //DEBUG_SERIAL_PRINTLN_FLASHSTRING("JSON msg bad!");
      return;
      }
    bptr += wrote;
    *bptr++ = crc; // Add 7-bit CRC for on-the-wire check.
    *bptr = 0xff; // Terminate message for TX.
#if 0 && defined(DEBUG)
    if(bptr - buf >= 64)
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("Too long for RFM2x: ");
      DEBUG_SERIAL_PRINT((int)(bptr - buf));
      DEBUG_SERIAL_PRINTLN();
      return;
      }
#endif
    // TODO: put in listen before TX to reduce collisions (CSMA).
    // Send it!
    RFM22RawStatsTX(false, buf, resumeRX, allowDoubleTX);
    }

#endif // defined(ALLOW_JSON_OUTPUT)

//DEBUG_SERIAL_PRINTLN_FLASHSTRING("Stats TX");
  }











// 'Elapsed minutes' count of minute/major cycles; cheaper than accessing RTC and not tied to real time.
static uint8_t minuteCount;

#if defined(ENABLE_BOILER_HUB)
// Ticks until locally-controlled boiler should be turned off; boiler should be on while this is positive.
// Ticks are the mail loop time, 1s or 2s.
// Used in hub mode only.
static uint16_t boilerCountdownTicks;
// Minutes since boiler last on as result of remote call for heat.
// Reducing listening if quiet for a while helps reduce self-heating temperature error
// (~2C as of 2013/12/24 at 100% RX, ~100mW heat dissipation in V0.2 REV1 box) and saves some energy.
// Time thresholds could be affected by eco/comfort switch.
#define RX_REDUCE_MIN_M 20 // Minimum minutes quiet before considering reducing RX duty cycle listening for call for heat; [1--255], 10--60 typical.
// IF DEFINED then give backoff threshold to minimise duty cycle.
// #define RX_REDUCE_MAX_M 240 // Minutes quiet before considering maximally reducing RX duty cycle; ]RX_REDUCE_MIN_M--255], 30--240 typical.
static uint8_t boilerNoCallM;
#endif


// Controller's view of Least Significiant Digits of the current (local) time, in this case whole seconds.
// See PICAXE V0.1/V0.09/DHD201302L0 code.
#define TIME_LSD_IS_BINARY // TIME_LSD is in binary (cf BCD).
#define TIME_CYCLE_S 60 // TIME_LSD ranges from 0 to TIME_CYCLE_S-1, also major cycle length.
static uint_fast8_t TIME_LSD; // Controller's notion of seconds within major cycle.

// Mask for Port D input change interrupts.
#define MASK_PD_BASIC 0b00000001 // Just RX.
#if defined(ENABLE_VOICE_SENSOR)
#if VOICE_NIRQ > 7
#error voice interrupt on wrong port
#endif
#define VOICE_INT_MASK (1 << (VOICE_NIRQ&7))
#define MASK_PD (MASK_PD_BASIC | VOICE_INT_MASK)
#else
#define MASK_PD MASK_PD_BASIC // Just RX.
#endif

void setupOpenTRV()
  {
  // Set up async edge interrupts.
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    //PCICR = 0x05;
    //PCMSK0 = 0b00000011; // PB; PCINT  0--7    (LEARN1 and Radio)
    //PCMSK1 = 0b00000000; // PC; PCINT  8--15
    //PCMSK2 = 0b00101001; // PD; PCINT 16--24   (LEARN2 and MODE, RX)
    PCICR = 0x4; // 0x4 enables PD/PCMSK2.
    PCMSK2 = MASK_PD; // PD; PCINT 16--24 (0b1 is PCINT16/RX)
    }

  // Do early 'wake-up' stats transmission if possible
  // when everything else is set up and ready.
  // Attempt to maximise chance of reception with a double TX.
  // Assume not in hub mode yet.
  // Send all possible formats.
  bareStatsTX(false, true, true);
  // Send stats repeatedly until all values pushed out (no 'changed' values unsent).
  do
    {
    nap(WDTO_120MS); // Sleep long enough for receiver to have a chance to process previous TX.
    bareStatsTX(false, true, false);
    } while(false); // (ss1.changedValue()); // FIXME.

#if defined(LOCAL_TRV) && defined(DIRECT_MOTOR_DRIVE_V1)
  // Signal some sort of life on waking up...
  ValveDirect.wiggle();
#endif

  // Set appropriate loop() values just before entering it.
  TIME_LSD = getSecondsLT();
  }

#if !defined(ALT_MAIN_LOOP) // Do not define handlers here when alt main is in use.
// Previous state of port D pins to help detect changes.
static volatile uint8_t prevStatePD;
// Interrupt service routine for PD I/O port transition changes (including RX).
ISR(PCINT2_vect)
  {
  const uint8_t pins = PIND;
  const uint8_t changes = pins ^ prevStatePD;
  prevStatePD = pins;

#if defined(ENABLE_VOICE_SENSOR)
//  // Voice detection is a falling edge.
//  // Handler routine not required/expected to 'clear' this interrupt.
//  // FIXME: ensure that Voice.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
//  if((changes & VOICE_INT_MASK) && !(pins & VOICE_INT_MASK))
  // Voice detection is a RISING edge.
  // Handler routine not required/expected to 'clear' this interrupt.
  // FIXME: ensure that Voice.handleInterruptSimple() is inlineable to minimise ISR prologue/epilogue time and space.
  if((changes & VOICE_INT_MASK) && (pins & VOICE_INT_MASK))
    { Voice.handleInterruptSimple(); }
#endif

  // TODO: MODE button and other things...

  // If an interrupt arrived from no other masked source then wake the CLI.
  // The will ensure that the CLI is active, eg from RX activity,
  // eg it is possible to wake the CLI subsystem with an extra CR or LF.
  // It is OK to trigger this from other things such as button presses.
  // FIXME: ensure that resetCLIActiveTimer() is inlineable to minimise ISR prologue/epilogue time and space.
  if(!(changes & MASK_PD & ~1)) { resetCLIActiveTimer(); }
  }
#endif








// Main loop for OpenTRV radiator control.
// Note: exiting and re-entering can take a little while, handling Arduino background tasks such as serial.
void loopOpenTRV()
  {
#if 0 && defined(DEBUG) // Indicate loop start.
  DEBUG_SERIAL_PRINT('L');
  DEBUG_SERIAL_PRINT(TIME_LSD);
  DEBUG_SERIAL_PRINTLN();
#endif


  // Set up some variables before sleeping to minimise delay/jitter after the RTC tick.
  bool showStatus = false; // Show status at end of loop?

  // Use the zeroth second in each minute to force extra deep device sleeps/resets, etc.
  const bool second0 = (0 == TIME_LSD);
  // Sensor readings are taken late in each minute (where they are taken)
  // and if possible noise and heat and light should be minimised in this part of each minute to improve readings.
//  const bool sensorReading30s = (TIME_LSD >= 30);
  // Sensor readings and (stats transmissions) are nominally on a 4-minute cycle.
  const uint8_t minuteFrom4 = (minuteCount & 3);
  // The 0th minute in each group of four is always used for measuring where possible (possibly amongst others)
  // and where possible locally-generated noise and heat and light should be minimised in this minute to give the best possible readings.
  // True if this is the first (0th) minute in each group of four.
  const bool minute0From4ForSensors = (0 == minuteFrom4);
  // True if this is the minute after all sensors should have been sampled.
  const bool minute1From4AfterSensors = (1 == minuteFrom4);

  // Note last-measured battery status.
  const bool batteryLow = Supply_mV.isSupplyVoltageLow();

  // Run some tasks less often when not demanding heat (at the valve or boiler), so as to conserve battery/energy.
  // Spare the batteries if they are low, or the unit is in FROST mode, or if the room/area appears to be vacant.
  // Stay responsive if the valve is open and/or we are otherwise calling for heat.
  const bool conserveBattery =
    (batteryLow || !inWarmMode() || Occupancy.longVacant()) &&
#if defined(ENABLE_BOILER_HUB)
    (0 == boilerCountdownTicks) && // Unless the boiler is off, stay responsive.
#endif
    (!NominalRadValve.isControlledValveReallyOpen()) &&  // Run at full speed until valve(s) should actually have shut and the boiler gone off.
    (!NominalRadValve.isCallingForHeat()); // Run at full speed until not nominally demanding heat, eg even during FROST mode or pre-heating.

  // Try if very near to end of cycle and thus causing an overrun.
  // Conversely, if not true, should have time to savely log outputs, etc.
  const uint8_t nearOverrunThreshold = GSCT_MAX - 8; // ~64ms/~32 serial TX chars of grace time...
  bool tooNearOverrun = false; // Set flag that can be checked later.

  // Is this unit currently in central hub listener mode?
  const bool hubMode = inHubMode();

#if defined(ENABLE_BOILER_HUB)
  // Check (early) for any remote stats arriving to dump.
  // This is designed to be easy to pick up by reading the serial output.
  // The output is terse to avoid taking too long and possibly delaying other stuff too far.
  // Avoid doing this at all if too near the end of the cycle and risking overrun,
  // leaving any message queued, hoping it does not get overwritten.
  // TODO: safely process more than one pending message if present.
  // TODO: move to process in a batch periodically, eg when CLI is due.
  if(getSubCycleTime() >= nearOverrunThreshold) { tooNearOverrun = true; }
  else
    {
    // Look for binary-format message.
    FullStatsMessageCore_t stats;
    getLastCoreStats(&stats);
    if(stats.containsID)
      {
      // Dump (remote) stats field '@<hexnodeID>;TnnCh[P;]'
      // where the T field shows temperature in C with a hex digit after the binary point indicated by C
      // and the optional P field indicates low power.
      serialPrintAndFlush(LINE_START_CHAR_RSTATS);
      serialPrintAndFlush((((uint16_t)stats.id0) << 8) | stats.id1, HEX);
      if(stats.containsTempAndPower)
        {
        serialPrintAndFlush(F(";T"));
        serialPrintAndFlush(stats.tempAndPower.tempC16 >> 4, DEC);
        serialPrintAndFlush('C');
        serialPrintAndFlush(stats.tempAndPower.tempC16 & 0xf, HEX);
        if(stats.tempAndPower.powerLow) { serialPrintAndFlush(F(";P")); } // Insert power-low field if needed.
        }
      if(stats.containsAmbL)
        {
        serialPrintAndFlush(F(";L"));
        serialPrintAndFlush(stats.ambL);
        }
      if(0 != stats.occ)
        {
        serialPrintAndFlush(F(";O"));
        serialPrintAndFlush(stats.occ);
        }
      serialPrintlnAndFlush();
      }
    // Check for JSON/text-format message if no binary message waiting.
    else
      {
      char buf[MSG_JSON_MAX_LENGTH+1];
      getLastJSONStats(buf);
      if('\0' != *buf)
        {
        // Dump contained JSON message as-is at start of line.
        serialPrintAndFlush(buf);
        serialPrintlnAndFlush();
        }
      }
    }
#endif

#if defined(ENABLE_BOILER_HUB)
  // IF IN CENTRAL HUB MODE: listen out for OpenTRV units calling for heat.
  // Power optimisation 1: when >> 1 TX cycle (of ~2mins) need not listen, ie can avoid enabling receiver.
  // Power optimisation 2: TODO: when (say) >>30m since last call for heat then only sample listen for (say) 3 minute in 10 (not at a TX cycle multiple).
  // TODO: These optimisation are more important when hub unit is running a local valve
  // to avoid temperature over-estimates from self-heating,
  // and could be disabled if no local valve is being run to provide better response to remote nodes.
  bool hubModeBoilerOn = false; // If true then remote call for heat is in progress.
#if defined(USE_MODULE_FHT8VSIMPLE)
  bool needsToEavesdrop = false; // By default assume no need to eavesdrop.
#endif
  if(hubMode)
    {
#if defined(USE_MODULE_FHT8VSIMPLE)
    // Final poll to to cover up to end of previous minor loop.
    // Keep time from here to following SetupToEavesdropOnFHT8V() as short as possible to avoid missing remote calls.
    FHT8VCallForHeatPoll();

    // Fetch and clear current pending sample house code calling for heat.
    const uint16_t hcRequest = FHT8VCallForHeatHeardGetAndClear();
    const bool heardIt = (hcRequest != ((uint16_t)~0));
    // Don't log call for hear if near overrun,
    // and leave any error queued for next time.
    if(getSubCycleTime() >= nearOverrunThreshold) { tooNearOverrun = true; }
    else
      {
      if(heardIt)
        {
//        DEBUG_SERIAL_TIMESTAMP();
//        DEBUG_SERIAL_PRINT(' ');
        serialPrintAndFlush(F("CfH ")); // Call for heat from 
        serialPrintAndFlush((hcRequest >> 8) & 0xff);
        serialPrintAndFlush(' ');
        serialPrintAndFlush(hcRequest & 0xff);
        serialPrintlnAndFlush();
        }
      else
        {
        // Check for error if nothing received.
        const uint8_t err = FHT8VLastRXErrGetAndClear();
        if(0 != err)
          {
          serialPrintAndFlush(F("!RXerr F"));
          serialPrintAndFlush(err);
          serialPrintlnAndFlush();
          }
        }
      }

    // Record call for heat, both to start boiler-on cycle and to defer need to listen again. 
    // Optimisation: may be able to stop RX if boiler is on for local demand (can measure local temp better: less self-heating).
    if(heardIt)
      {
      if(0 == boilerCountdownTicks)
        {
        if(getSubCycleTime() >= nearOverrunThreshold) { tooNearOverrun = true; }
        else { serialPrintlnAndFlush(F("RCfH1")); } // Remote call for heat on.
        }
      boilerCountdownTicks = getMinBoilerOnMinutes() * (60/MAIN_TICK_S);
      boilerNoCallM = 0; // No time has passed since the last call.
      }
    // Else count down towards boiler off.
    else if(boilerCountdownTicks > 0)
      {
      if(0 == --boilerCountdownTicks)
        {
        if(getSubCycleTime() >= nearOverrunThreshold) { tooNearOverrun = true; }
        else { serialPrintlnAndFlush(F("RCfH0")); } // Remote call for heat off
        }
      }
    // Else already off so count up quiet minutes...
    else if(second0 && (boilerNoCallM < (uint8_t)~0)) { ++boilerNoCallM; }         

    // Turn boiler output on or off in response to calls for heat.
    hubModeBoilerOn = (boilerCountdownTicks > 0);

    // If not running a local TRV, and thus without local temperature measurement problems from self-heating,
    // then just listen all the time for maximum simplicity and responsiveness at some cost in extra power consumption.
    // (At least as long as power is not running low for some reason.)
    if(!localFHT8VTRVEnabled() && !batteryLow)
      { needsToEavesdrop = true; }
    // Try to avoid listening in the 'quiet' sensor minute in order to minimise noise and power consumption and self-heating.
    // Optimisation: if just heard a call need not listen on this next cycle.
    // Optimisation: if boiler timeout is a long time away (>> one FHT8V TX cycle, ~2 minutes excl quiet minute), then can avoid listening for now.
    //    Longish period without any RX listening may allow hub unit to cool and get better sample of local temperature if marginal.
    // Aim to listen in one stretch for greater than full FHT8V TX cycle of ~2m to avoid missing a call for heat.
    // MUST listen for all of final 2 mins of boiler-on to avoid missing TX (without forcing boiler over-run).
    else if((boilerCountdownTicks <= ((MAX_FHT8V_TX_CYCLE_HS+1)/(2*MAIN_TICK_S))) && // Don't miss a final TX that would keep the boiler on...
       (boilerCountdownTicks != 0)) // But don't force unit to listen/RX all the time if no recent call for heat.
      { needsToEavesdrop = true; }
    else if((!heardIt) &&
       (!minute0From4ForSensors) &&
       (boilerCountdownTicks <= (RX_REDUCE_MIN_M*(60/MAIN_TICK_S)))) // Listen eagerly for fresh calls for heat for last few minutes before turning boiler off.
      {
#if defined(RX_REDUCE_MAX_M) && defined(LOCAL_TRV)
      // Skip the minute before the 'quiet' minute also in very quiet mode to improve local temp measurement.
      // (Should still catch at least one TX per 4 minutes at worst.)
      needsToEavesdrop =
          ((boilerNoCallM <= RX_REDUCE_MAX_M) || (3 != (minuteCount & 3)));
#else
      needsToEavesdrop = true;
#endif
      }

#endif
    }
#endif

#if defined(ENABLE_BOILER_HUB)
#if defined(USE_MODULE_FHT8VSIMPLE)
  // Act on eavesdropping need, setting up or clearing down hooks as required.
  if(needsToEavesdrop)
    {
    // Ensure radio is in RX mode rather than standby, and possibly hook up interrupts if available (REV1 board).
    const bool startedRX = SetupToEavesdropOnFHT8V(second0); // Start listening (if not already so).
#if 0 && defined(DEBUG)
    if(startedRX) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("STARTED eavesdropping"); }
#endif
#if 1 && defined(DEBUG)
    const uint16_t dropped = getInboundStatsQueueOverrun();
    static uint16_t oldDropped;
    if(dropped != oldDropped)
      {
      DEBUG_SERIAL_PRINT_FLASHSTRING("?DROPPED stats: ");
      DEBUG_SERIAL_PRINT(dropped);
      DEBUG_SERIAL_PRINTLN();
      oldDropped = dropped;
      }
#endif
#if 0 && defined(DEBUG)
    DEBUG_SERIAL_PRINT_FLASHSTRING("hub listen, on/cd ");
    DEBUG_SERIAL_PRINT(boilerCountdownTicks);
    DEBUG_SERIAL_PRINT_FLASHSTRING("t quiet ");
    DEBUG_SERIAL_PRINT(boilerNoCallM);
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("m");
#endif
    }
  else
    {
    // Power down and clear radio state (if currently eavesdropping).
    StopEavesdropOnFHT8V(second0);
    // Clear any RX state so that nothing stale is carried forward.
    FHT8VCallForHeatHeardGetAndClear();
    }
#endif
#endif


  // Set BOILER_OUT as appropriate for local and/or remote calls for heat.
  // FIXME: local valve-driven boiler on does not obey normal on/off run-time rules.
#if defined(ENABLE_BOILER_HUB)
  fastDigitalWrite(OUT_HEATCALL, ((hubModeBoilerOn || NominalRadValve.isControlledValveReallyOpen()) ? HIGH : LOW));
#else
  fastDigitalWrite(OUT_HEATCALL, NominalRadValve.isControlledValveReallyOpen() ? HIGH : LOW);
#endif


  // Sleep in low-power mode (waiting for interrupts) until seconds roll.
  // NOTE: sleep at the top of the loop to minimise timing jitter/delay from Arduino background activity after loop() returns.
  // DHD20130425: waking up from sleep and getting to start processing below this block may take >10ms.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("*E"); // End-of-cycle sleep.
#endif
  // Ensure that serial I/O is off.
  powerDownSerial();
  // Power down most stuff (except radio for hub RX).
  minimisePowerWithoutSleep();
  uint_fast8_t newTLSD;
  while(TIME_LSD == (newTLSD = getSecondsLT()))
    {
#if defined(ENABLE_BOILER_HUB) && defined(USE_MODULE_FHT8VSIMPLE) // Deal with FHT8V eavesdropping if needed.
    // Poll for RX of remote calls-for-heat if needed.
    if(needsToEavesdrop) { nap30AndPoll(); continue; }
#endif
#if defined(USE_MODULE_RFM22RADIOSIMPLE) // Force radio to power-saving standby state if appropriate.
    // Force radio to known-low-power state from time to time (not every time to avoid unnecessary SPI work, LED flicker, etc.)
    if(batteryLow || second0) { RFM22ModeStandbyAndClearState(); }
#endif
    sleepUntilInt(); // Normal long minimal-power sleep until wake-up interrupt.
    }
  TIME_LSD = newTLSD;
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("*S"); // Start-of-cycle wake.
#endif

#if defined(ENABLE_BOILER_HUB) && defined(USE_MODULE_FHT8VSIMPLE) // Deal with FHT8V eavesdropping if needed.
  // Check RSSI...
  if(needsToEavesdrop)
    {
    const uint8_t rssi = RFM22RSSI();
    static uint8_t lastRSSI;
    if((rssi > 0) && (lastRSSI != rssi))
      {
      lastRSSI = rssi;
      addEntropyToPool(rssi, 0); // Probably some real entropy but don't assume it.
#if 0 && defined(DEBUG)
      DEBUG_SERIAL_PRINT_FLASHSTRING("RSSI=");
      DEBUG_SERIAL_PRINT(rssi);
      DEBUG_SERIAL_PRINTLN();
#endif
      }
    }
#endif

#if 0 && defined(DEBUG) // Show CPU cycles.
  DEBUG_SERIAL_PRINT('C');
  DEBUG_SERIAL_PRINT(cycleCountCPU());
  DEBUG_SERIAL_PRINTLN();
#endif


  // START LOOP BODY
  // ===============


  // Warn if too near overrun before.
  if(tooNearOverrun) { serialPrintlnAndFlush(F("?near overrun")); }


  // Get current power supply voltage.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("Vcc: ");
  DEBUG_SERIAL_PRINT(Supply_mV.read());
  DEBUG_SERIAL_PRINTLN_FLASHSTRING("mV");
#endif

  // Dump port status re interrupt handling.
#if 0 && defined(DEBUG)
  DEBUG_SERIAL_PRINT_FLASHSTRING("PORTS: D=");
  DEBUG_SERIAL_PRINTFMT(prevStatePD, HEX);
  DEBUG_SERIAL_PRINTLN();
#endif





#if defined(USE_MODULE_FHT8VSIMPLE)
  // Try for double TX for more robust conversation with valve unless:
  //   * battery is low
  //   * the valve is not required to be wide open (ie a reasonable temperature is currently being maintained).
  //   * this is a hub and has to listen as much as possible
  // to conserve battery and bandwidth.
  const bool doubleTXForFTH8V = !conserveBattery && !hubMode && (NominalRadValve.get() >= 50);
  // FHT8V is highest priority and runs first.
  // ---------- HALF SECOND #0 -----------
  bool useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8VPollSyncAndTX_First(doubleTXForFTH8V); // Time for extra TX before UI.
//  if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@0"); }
#endif


  // High-priority UI handing, every other/even second.
  // Show status if the user changed something significant.
  // Must take ~300ms or less so as not to run over into next half second if two TXs are done.
  bool recompute = false; // Set true an extra recompute of target temperature should be done.
#if !defined(TWO_S_TICK_RTC_SUPPORT)
  if(0 == (TIME_LSD & 1))
#endif
    {
    if(tickUI(TIME_LSD))
      {
      showStatus = true;
      recompute = true;
      }
    }

  if(recompute || veryRecentUIControlUse())
    {
    // Force immediate recompute of target temperature for (UI) responsiveness.
    NominalRadValve.computeTargetTemperature();
    }


#if defined(USE_MODULE_FHT8VSIMPLE)
  if(useExtraFHT8VTXSlots)
    {
    // Time for extra TX before other actions, but don't bother if minimising power in frost mode.
    // ---------- HALF SECOND #1 -----------
    useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8VPollSyncAndTX_Next(doubleTXForFTH8V); 
//    if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@1"); }
    }
#endif




  // DO SCHEDULING

  // Once-per-minute tasks: all must take << 0.3s.
  // Run tasks spread throughout the minute to be as kind to batteries (etc) as possible.
  // Only when runAll is true run less-critical tasks that be skipped sometimes when particularly conserving energy.
  // TODO: coordinate temperature reading with time when radio and other heat-generating items are off for more accurate readings.
  // TODO: ensure only take ambient light reading at times when all LEDs are off.
  const bool runAll = (!conserveBattery) || minute0From4ForSensors;

  switch(TIME_LSD) // With TWO_S_TICK_RTC_SUPPORT only even seconds are available.
    {
    case 0:
      {
      // Tasks that must be run every minute.
      ++minuteCount;
      checkUserSchedule(); // Force to user's programmed settings, if any, at the correct time.
      // Ensure that the RTC has been persisted promptly when necessary.
      persistRTC();
      break;
      }

    // Churn/reseed PRNG(s) a little to improve unpredictability in use: should be lightweight.
    case 2: { if(runAll) { seedRNG8(minuteCount ^ cycleCountCPU() ^ (uint8_t)Supply_mV.get(), _getSubCycleTime() ^ (uint8_t)AmbLight.get(), (uint8_t)TemperatureC16.get()); } break; }
    // Force read of supply/battery voltage; measure and recompute status (etc) less often when already thought to be low, eg when conserving.
    case 4: { if(runAll) { Supply_mV.read(); } break; }

    // Regular transmission of stats if NOT driving a local valve (else stats can be piggybacked onto that).
    case 10:
      {
      if(!enableTrailingStatsPayload()) { break; } // Not allowed to send stuff like this.
#if defined(USE_MODULE_FHT8VSIMPLE)
      // Avoid transmit conflict with FS20; just drop the slot.
      // We should possibly choose between this and piggybacking stats to avoid busting duty-cycle rules.
      if(localFHT8VTRVEnabled() && useExtraFHT8VTXSlots) { break; }
#endif

      // Generally only attempt stats TX in the minute after all sensors should have been polled (so that readings are fresh).
      if(minute1From4AfterSensors ||
        (!batteryLow && (0 == (0x24 & randRNG8())))) // Occasional additional TX when not conserving power.
        {
        pollIO(); // Deal with any pending I/O.
        // Sleep randomly up to 128ms to spread transmissions and thus help avoid collisions.
        sleepLowPowerLessThanMs(1 + (randRNG8() & 0x7f));
        pollIO(); // Deal with any pending I/O.
        // Send it!
        // Try for double TX for extra robustness unless:
        //   * this is a speculative 'extra' TX
        //   * battery is low
        //   * this node is a hub so needs to listen as much as possible
        // This doesn't generally/always need to send binary/both formats
        // if this is controlling a local FHT8V on which the binary stats can be piggybacked.
        // Ie, if doesn't have a local TRV then it must send binary some of the time.
        const bool doBinary = !localFHT8VTRVEnabled() && randRNG8NextBoolean();
        bareStatsTX(hubMode, minute1From4AfterSensors && !batteryLow && !hubMode && ss1.changedValue(), doBinary);
        }
      break;
      }


// SENSOR READ AND STATS
//
// All external sensor reads should be in the second half of the minute (>=32) if possible.
// This is to have them as close to stats collection at the end of the minute as possible.
// Also all sources of noise, self-heating, etc, may be turned off for the 'sensor read minute'
// and thus will have diminished by this point.

#ifdef ENABLE_VOICE_SENSOR
    // Poll voice detection sensor at a fixed rate.
    case 46: { Voice.read(); break; }
#endif

#ifdef TEMP_POT_AVAILABLE
    // Sample the user-selected WARM temperature target at a fixed rate.
    // This allows the unit to stay reasonably responsive to adjusting the temperature dial.
    case 48: { TempPot.read(); break; }
#endif

    // Read all environmental inputs, late in the cycle.
#ifdef HUMIDITY_SENSOR_SUPPORT
    // Sample humidity.
    case 50: { if(runAll) { RelHumidity.read(); } break; }
#endif

    // Poll ambient light level at a fixed rate.
    // This allows the unit to respond consistently to (eg) switching lights on (eg TODO-388).
    case 52: { AmbLight.read(); break; }

    // At a hub, sample temperature regularly as late as possible in the minute just before recomputing valve position.
    // Force a regular read to make stats such as rate-of-change simple and to minimise lag.
    // TODO: optimise to reduce power consumption when not calling for heat.
    // TODO: optimise to reduce self-heating jitter when in hub/listen/RX mode.
    case 54: { TemperatureC16.read(); break; }
//    // A regular (slow) read is forced if filtering is on to reduce jitter in the results.
//    case 54: { if((hubMode || TemperatureC16.isFilteringOn()) ? minute0From4ForSensors : runAll) { TemperatureC16.read(); } break; }

    // Compute targets and heat demand based on environmental inputs and occupancy.
    // This should happen as soon after the latest readings as possible (temperature especially).
    case 56:
      {
#ifdef OCCUPANCY_SUPPORT
      // Update occupancy status (fresh for target recomputation) at a fixed rate.
      Occupancy.read();
#endif

      // Recompute target, valve position and call for heat, etc.
      // Should be called once per minute to work correctly.
      NominalRadValve.read();

#if defined(USE_MODULE_FHT8VSIMPLE)
      // If there was a change in target valve position,
      // or periodically in the minute after all sensors should have been read,
      // precompute some or all of any outgoing frame/stats/etc ready for the next transmission.
      if(NominalRadValve.isValveMoved() ||
         (minute1From4AfterSensors && enableTrailingStatsPayload()))
        {
        if(localFHT8VTRVEnabled()) { FHT8VCreateValveSetCmdFrame(); }
        }
#endif

#if defined(ENABLE_BOILER_HUB)
      // Track how long since remote call for heat last heard.
      if(hubMode)
        {
        if(boilerCountdownTicks != 0)
          {
#if 1 && defined(DEBUG)
          DEBUG_SERIAL_PRINT_FLASHSTRING("Boiler on, s: ");
          DEBUG_SERIAL_PRINT(boilerCountdownTicks * MAIN_TICK_S);
          DEBUG_SERIAL_PRINTLN();
#endif
          }
        }
#endif

      // Show current status if appropriate.
      if(runAll) { showStatus = true; }
      break;
      }

    // Stats samples; should never be missed.
    case 58:
      {
      // Take full stats sample as near the end of the hour as reasonably possible (without danger of overrun),
      // and with other optional non-full samples evenly spaced throughout the hour (if not low on battery).
      // A small even number of samples (or 1 sample) is probably most efficient.
      if(minute0From4ForSensors) // Use lowest-noise samples just taken in the special 0 minute out of each 4.
        {
        const uint_least8_t mm = getMinutesLT();
        switch(mm)
          {
          case 26: case 27: case 28: case 29:
            { if(!batteryLow) { sampleStats(false); } break; } // Skip sub-samples if short of energy.
          case 56: case 57: case 58: case 59:
            { sampleStats(true); break; } // Always take the full sample at the end of each hour.
          }
        }
      break;
      }
    }

#if defined(USE_MODULE_FHT8VSIMPLE) && defined(TWO_S_TICK_RTC_SUPPORT)
  if(useExtraFHT8VTXSlots)
    {
    // ---------- HALF SECOND #2 -----------
    useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8VPollSyncAndTX_Next(doubleTXForFTH8V); 
//    if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@2"); }
    }
#endif

  // Generate periodic status reports.
  if(showStatus) { serialStatusReport(); }

#if defined(USE_MODULE_FHT8VSIMPLE) && defined(TWO_S_TICK_RTC_SUPPORT)
  if(useExtraFHT8VTXSlots)
    {
    // ---------- HALF SECOND #3 -----------
    useExtraFHT8VTXSlots = localFHT8VTRVEnabled() && FHT8VPollSyncAndTX_Next(doubleTXForFTH8V); 
//    if(useExtraFHT8VTXSlots) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("ES@3"); }
    }
#endif

  // Command-Line Interface (CLI) polling.
  // If a reasonable chunk of the minor cycle remains after all other work is done
  // AND the CLI is / should be active OR a status line has just been output
  // then poll/prompt the user for input
  // using a timeout which should safely avoid overrun, ie missing the next basic tick,
  // and which should also allow some energy-saving sleep.
#if 1 && defined(SUPPORT_CLI)
  const bool humanCLIUse = isCLIActive(); // Keeping CLI active for human interaction rather than for automated interaction.
  if(showStatus || humanCLIUse)
    {
    const uint8_t sct = getSubCycleTime();
    const uint8_t listenTime = max(GSCT_MAX/16, CLI_POLL_MIN_SCT);
    if(sct < (GSCT_MAX - 2*listenTime))
      // Don't listen beyond the last 16th of the cycle,
      // or a minimal time if only prodding for interaction with automated front-end,
      // as listening for UART RX uses lots of power.
      { pollCLI(humanCLIUse ? (GSCT_MAX-listenTime) : (sct+CLI_POLL_MIN_SCT)); }
    }
#endif



#if 0 && defined(DEBUG)
  const int tDone = getSubCycleTime();
  if(tDone > 1) // Ignore for trivial 1-click time.
    {
    DEBUG_SERIAL_PRINT_FLASHSTRING("done in "); // Indicates what fraction of available loop time was used / 256.
    DEBUG_SERIAL_PRINT(tDone);
    DEBUG_SERIAL_PRINT_FLASHSTRING(" @ ");
    DEBUG_SERIAL_TIMESTAMP();
    DEBUG_SERIAL_PRINTLN();
    }
#endif

  // Detect and handle (actual or near) overrun, if it happens, though it should not.
  if(TIME_LSD != getSecondsLT())
    {
    // Increment the overrun counter (stored inverted, so 0xff initialised => 0 overruns).
    const uint8_t orc = 1 + ~eeprom_read_byte((uint8_t *)EE_START_OVERRUN_COUNTER);
    eeprom_smart_update_byte((uint8_t *)EE_START_OVERRUN_COUNTER, ~orc);
#if 1 && defined(DEBUG)
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("!ERROR: loop overrun");
//    DEBUG_SERIAL_PRINT(orc);
//    DEBUG_SERIAL_PRINTLN();
#endif
#if defined(USE_MODULE_FHT8VSIMPLE)
    FHT8VSyncAndTXReset(); // Assume that sync with valve may have been lost, so re-sync.
#endif
    TIME_LSD = getSecondsLT(); // Prepare to sleep until start of next full minor cycle.
    }
#if 0 && defined(DEBUG) // Expect to pick up near overrun at start of next loop.
  else if(getSubCycleTime() >= nearOverrunThreshold)
    {
    DEBUG_SERIAL_PRINTLN_FLASHSTRING("?O"); // Near overrun.  Note 2ms/char to send...
    }
#endif
  }

