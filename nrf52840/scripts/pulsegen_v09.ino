// pulse_engine: UNO R4 pulse generator
//
// What it does
// - Generates accurate periodic pulses using hardware PWM (LOOP/GATE).
// - Generates single pulses (one-shot) using GPIO + hardware timer (TRIG/PULSE and "pulse" command).
// - Controlled over Serial (115200, newline-terminated, case-insensitive).
//
// Pins / Electrical
// - OUT_PIN  (D9): Output signal pin.
//     - In LOOP/GATE: driven by hardware PWM peripheral.
//     - In TRIG/PULSE and "pulse": driven as a GPIO HIGH then forced LOW by a one-shot timer callback.
// - TRIG_PIN (D2): External trigger / gate input pin (INPUT_PULLDOWN).
//     - In GATE: PWM runs only while TRIG_PIN is HIGH (arm must be 1).
//     - In TRIG: each rising edge produces one output pulse (arm must be 1).
//     - In PULSE: only the next rising edge produces one output pulse, then ignores further edges until re-primed.
//
// Limits + UI (enforced + warnings)
// - LOOP / GATE / NONE (PWM-based):
//     PER >= PER_MIN_PWM_US
//     WID in [1 .. PER-1]
// - TRIG / PULSE (single edge pulse, GPIO + timer):
//     User-facing WID >= WID_MIN_USER_ONESHOT_US (shown in status)
//     Actual timer width = max(1, (UserWID - ONESHOT_UI_OFFSET_US))
//     This compensates the ~fixed overhead you observed (pulse ~= requested + ~10us).
//     PER is stored/displayed but NOT used to generate the one-shot pulse.
// - "pulse" command (immediate pulse NOW) uses the same TRIG/PULSE width behavior.
//
// Modes
// - mode none  : output off (idle)
// - mode loop  : continuous PWM. arm=0 => start now. arm=1 => start on rising edge.
// - mode gate  : PWM while TRIG pin is HIGH (arm must be 1). Stops when TRIG goes LOW.
// - mode trig  : each rising edge generates ONE pulse (single pulse).
// - mode pulse : only NEXT rising edge generates ONE pulse, then ignores further edges until re-primed.
// - arm 1/0    : enable/disable external trigger usage
// - trig/trigger: software rising edge (only when arm=1)
// - pulse      : immediate pulse NOW (independent of mode)
// - stop/q     : panic stop, forces output low, sets mode none, arm 0, state idle.
//
// Protocol (115200, newline, case-insensitive)
// - ping | status | help | -h | --help | ? | led on/off
// - per <us> | wid <us>
// - mode none|loop|gate|trig|pulse   (also: mod ..., mode:..., mod:...)
// - arm 1/0
// - trig / trigger
// - pulse
// - stop / q
//
// Status hints
// - For modes that require ARM=1 (GATE/TRIG/PULSE), status prints HINT=ARM1_REQUIRED if ARM=0.
// - For PULSE mode, status prints HINT=PULSE_PRIMED or HINT=PULSE_NOT_PRIMED to reduce "why didn't it fire" moments.

#include <Arduino.h>
#include "pwm.h"
#include "FspTimer.h"

// -------------------- Pins --------------------
// D5 is a good choice because it is PWM-capable on UNO R4, and still usable as GPIO for one-shots.
static const int OUT_PIN  = 5;  // Output signal (PWM in periodic modes, GPIO for one-shot pulses)
// D3 is used as the trigger/gate input because it supports interrupts and is easy to wire.
static const int TRIG_PIN = 3;  // Trigger/gate input (INPUT_PULLDOWN)

// -------------------- Serial input --------------------
static String line;

// -------------------- Parameters (stored, user-facing) --------------------
static volatile uint32_t per_us = 1000;  // Period in microseconds (PWM modes)
static volatile uint32_t wid_us = 50;    // Pulse width in microseconds (PWM and one-shot user-facing)

// -------------------- Limits --------------------
static const uint32_t PER_MIN_PWM_US            = 2;

// TRIG/PULSE width UI compensation:
// - user sees/sets wid_us (min 10)
// - actual timer uses max(1, wid_us - 10)
static const uint32_t WID_MIN_USER_ONESHOT_US   = 10;
static const uint32_t ONESHOT_UI_OFFSET_US      = 10;
static const uint32_t WID_MIN_EFFECTIVE_US      = 1;

// -------------------- User-selected mode --------------------
enum UserMode : uint8_t {
  MODE_NONE = 0,
  MODE_LOOP,
  MODE_GATE,
  MODE_TRIG,
  MODE_PULSE
};
static volatile UserMode user_mode = MODE_NONE;

// -------------------- Internal state --------------------
enum State : uint8_t {
  STATE_IDLE = 0,
  STATE_LOOP_RUNNING,
  STATE_GATE_RUNNING,
  STATE_PULSE_ACTIVE
};
static volatile State state_now = STATE_IDLE;

// Trigger gating + flags
static volatile bool armed = false;        // When false, external trigger/gate is ignored
static volatile bool pulse_ready = false;  // Used only in MODE_PULSE: next rising edge is allowed when true

static volatile bool soft_trigger_pending = false;

// Interrupt-captured trigger level/edges
static volatile bool trig_level = false;
static volatile bool trig_rise_pending = false;
static volatile bool trig_fall_pending = false;

// PWM update request (only while loop/gate PWM is running)
static volatile bool pwm_update_pending = false;

// Tracks whether PWM is enabled
static volatile bool pwm_active = false;

// -------------------- Command parsing helpers --------------------
static String normalize_command(String s) {
  s.trim();
  s.replace(':', ' ');
  while (s.indexOf("  ") >= 0) s.replace("  ", " ");
  return s;
}

static bool equals_ci(const String& s, const char* w) {
  if (s.length() != (int)strlen(w)) return false;
  for (int i = 0; i < s.length(); i++) {
    char a = s.charAt(i);
    char b = w[i];
    if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
    if (a != b) return false;
  }
  return true;
}

static bool starts_with_ci(const String& s, const char* pfx) {
  int n = (int)strlen(pfx);
  if (s.length() < n) return false;
  for (int i = 0; i < n; i++) {
    char a = s.charAt(i);
    char b = pfx[i];
    if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
    if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
    if (a != b) return false;
  }
  return true;
}

static long to_int_from(const String& s, int start_index) {
  return s.substring(start_index).toInt();
}

// -------------------- Helpers: mode/state strings --------------------
static inline const char* mode_str(UserMode m) {
  switch (m) {
    case MODE_LOOP:  return "LOOP";
    case MODE_GATE:  return "GATE";
    case MODE_TRIG:  return "TRIG";
    case MODE_PULSE: return "PULSE";
    default:         return "NONE";
  }
}

static inline const char* state_str(State s) {
  switch (s) {
    case STATE_LOOP_RUNNING: return "LOOP";
    case STATE_GATE_RUNNING: return "GATE";
    case STATE_PULSE_ACTIVE: return "PULSE";
    default:                 return "IDLE";
  }
}

// -------------------- Parameter normalization --------------------
static inline bool mode_is_pwm(UserMode m) {
  return (m == MODE_NONE) || (m == MODE_LOOP) || (m == MODE_GATE);
}

static inline bool mode_is_oneshot(UserMode m) {
  return (m == MODE_TRIG) || (m == MODE_PULSE);
}

static void warn_per_pwm_clamped(uint32_t new_per) {
  Serial.print("WARN: PER min=");
  Serial.print((unsigned long)PER_MIN_PWM_US);
  Serial.print("us in LOOP/GATE/NONE (clamped to ");
  Serial.print((unsigned long)new_per);
  Serial.println("us)");
}

static void warn_wid_pwm_clamped(uint32_t new_wid, uint32_t per) {
  Serial.print("WARN: WID range=[1..PER-1] in LOOP/GATE/NONE (clamped to ");
  Serial.print((unsigned long)new_wid);
  Serial.print("us, PER=");
  Serial.print((unsigned long)per);
  Serial.println("us)");
}

static void warn_wid_oneshot_clamped(uint32_t new_wid) {
  Serial.print("WARN: WID min=");
  Serial.print((unsigned long)WID_MIN_USER_ONESHOT_US);
  Serial.print("us in TRIG/PULSE (clamped to ");
  Serial.print((unsigned long)new_wid);
  Serial.println("us)");
}

// Normalize stored per_us/wid_us for a target mode; optionally print warnings.
static void normalize_params_for_mode(UserMode target_mode, bool print_warnings) {
  uint32_t p = (uint32_t)per_us;
  uint32_t w = (uint32_t)wid_us;

  if (mode_is_pwm(target_mode)) {
    if (p < PER_MIN_PWM_US) {
      p = PER_MIN_PWM_US;
      if (print_warnings) warn_per_pwm_clamped(p);
    }
    if (w < 1) {
      w = 1;
      if (print_warnings) warn_wid_pwm_clamped(w, p);
    }
    if (w >= p) {
      w = (p > 1) ? (p - 1) : 1;
      if (print_warnings) warn_wid_pwm_clamped(w, p);
    }
  } else {
    // TRIG/PULSE: user-facing minimum width
    if (w < WID_MIN_USER_ONESHOT_US) {
      w = WID_MIN_USER_ONESHOT_US;
      if (print_warnings) warn_wid_oneshot_clamped(w);
    }
    // PER is left as-is (stored/displayed), not used for oneshot generation.
  }

  per_us = p;
  wid_us = w;
}

// Convert user-facing WID to effective timer width for TRIG/PULSE/PULSE-now
static inline uint32_t oneshot_effective_width_us(uint32_t user_wid) {
  // Defend against unexpected values.
  if (user_wid < WID_MIN_USER_ONESHOT_US) user_wid = WID_MIN_USER_ONESHOT_US;

  uint32_t eff = 0;
  if (user_wid > ONESHOT_UI_OFFSET_US) eff = user_wid - ONESHOT_UI_OFFSET_US;
  else eff = 0;

  if (eff < WID_MIN_EFFECTIVE_US) eff = WID_MIN_EFFECTIVE_US;
  return eff;
}

// -------------------- Hardware PWM (LOOP/GATE) --------------------
static PwmOut pwm_out(OUT_PIN);

static void pwm_stop_force_low_foreground() {
  // Stop PWM if it is running, then force OUT_PIN low deterministically.
  if (pwm_active) {
    pwm_out.end();
    pwm_active = false;
  }
  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, LOW);
}

static void pwm_start_or_restart_foreground(uint32_t per, uint32_t wid) {
  // Restart is used for parameter changes and mode transitions.
  if (pwm_active) {
    pwm_out.end();
    pwm_active = false;
    delayMicroseconds(5);
  }

  bool ok = pwm_out.begin(per, wid, false, TIMER_SOURCE_DIV_1);
  if (!ok) {
    // Fail-safe: ensure output is quiet if PWM begin fails.
    pwm_out.end();
    pwm_active = false;
    pinMode(OUT_PIN, OUTPUT);
    digitalWrite(OUT_PIN, LOW);
  } else {
    pwm_active = true;
  }
}

// -------------------- One-shot pulse generator (GPIO + timer) --------------------
static FspTimer width_timer;
static int8_t   width_timer_index = -1;
static volatile bool width_timer_ready = false;
static volatile uint32_t width_timer_clk_hz = 0;

static volatile bool pulse_now_active = false;
static volatile bool restore_pwm_after_pulse_now = false;

static inline uint32_t us_to_counts(uint32_t clk_hz, uint32_t us) {
  if (us == 0) us = 1;
  uint64_t counts = ((uint64_t)clk_hz * (uint64_t)us) / 1000000ULL;
  if (counts < 1) counts = 1;
  if (counts > 0xFFFFFFFFULL) counts = 0xFFFFFFFFULL;
  return (uint32_t)counts;
}

// Timer callback ends the one-shot pulse by forcing OUT_PIN low.
// Keep this callback short and deterministic.
void width_timer_cb(timer_callback_args_t __attribute((unused)) *p_args) {
  digitalWrite(OUT_PIN, LOW);
  pulse_now_active = false;
}

static bool init_width_timer() {
  // One-shot timer used to end pulses. The output rising edge is done in foreground;
  // the falling edge is done in this timer callback for consistent timing.
  if (width_timer_ready) return true;

  uint8_t timer_type = GPT_TIMER;
  int8_t idx = FspTimer::get_available_timer(timer_type);
  if (idx < 0) idx = FspTimer::get_available_timer(timer_type, true);
  if (idx < 0) return false;

  FspTimer::force_use_of_pwm_reserved_timer();
  width_timer_index = idx;

  uint32_t dummy_counts = 1000;
  if (!width_timer.begin(TIMER_MODE_ONE_SHOT, GPT_TIMER, (uint8_t)width_timer_index,
                         dummy_counts, 0, TIMER_SOURCE_DIV_1, width_timer_cb, nullptr)) {
    return false;
  }
  if (!width_timer.setup_overflow_irq()) return false;
  if (!width_timer.open()) return false;

  width_timer_clk_hz = width_timer.get_freq_hz();
  if (width_timer_clk_hz == 0) return false;

  width_timer_ready = true;
  return true;
}

static void start_pulse_now_user_wid(uint32_t user_requested_wid, bool print_warn_if_clamped) {
  // Produces an immediate one-shot pulse on OUT_PIN using the UI-compensated width rules.
  // This is used by:
  // - "pulse" command (immediate pulse now)
  // - MODE_TRIG and MODE_PULSE edge-triggered output
  if (!width_timer_ready) return;

  // Enforce user-facing minimum width even if current mode is PWM/NONE.
  uint32_t user_wid = user_requested_wid;
  if (user_wid < WID_MIN_USER_ONESHOT_US) {
    user_wid = WID_MIN_USER_ONESHOT_US;
    if (print_warn_if_clamped) warn_wid_oneshot_clamped(user_wid);
  }

  uint32_t effective_wid = oneshot_effective_width_us(user_wid);

  // Detach PWM safely if it is active, so the one-shot has exclusive control of OUT_PIN.
  if (pwm_active) {
    restore_pwm_after_pulse_now = true;
    pwm_stop_force_low_foreground();
  } else {
    restore_pwm_after_pulse_now = false;
    pinMode(OUT_PIN, OUTPUT);
    digitalWrite(OUT_PIN, LOW);
  }

  // Start the pulse: rising edge now, falling edge via one-shot timer.
  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, HIGH);
  pulse_now_active = true;

  width_timer.set_period(us_to_counts(width_timer_clk_hz, effective_wid));
  width_timer.reset();
  width_timer.start();
}

// -------------------- Apply selection to actual outputs (foreground only) --------------------
static void apply_selection_foreground() {
  // Do not alter hardware while an active one-shot is in progress.
  if (pulse_now_active) return;

  normalize_params_for_mode((UserMode)user_mode, false);

  // For one-shot modes, the default output state is LOW unless a pulse fires.
  if (mode_is_oneshot((UserMode)user_mode)) {
    pwm_stop_force_low_foreground();
    state_now = STATE_IDLE;
    return;
  }

  if (user_mode == MODE_NONE) {
    pwm_stop_force_low_foreground();
    state_now = STATE_IDLE;
    return;
  }

  if (user_mode == MODE_LOOP) {
    if (!armed) {
      // Free-run PWM.
      pwm_start_or_restart_foreground((uint32_t)per_us, (uint32_t)wid_us);
      state_now = pwm_active ? STATE_LOOP_RUNNING : STATE_IDLE;
    } else {
      // Armed loop starts on next rising edge; keep output quiet until then.
      pwm_stop_force_low_foreground();
      state_now = STATE_IDLE;
    }
    return;
  }

  if (user_mode == MODE_GATE) {
    // GATE mode output depends on trigger level and is handled in loop(); keep output quiet here.
    if (!armed) {
      pwm_stop_force_low_foreground();
      state_now = STATE_IDLE;
      return;
    }
    pwm_stop_force_low_foreground();
    state_now = STATE_IDLE;
    return;
  }
}

// -------------------- Status / Help --------------------
static const char* status_hint() {
  // Lightweight operator hints for common "nothing happens" situations.
  UserMode m = (UserMode)user_mode;
  if ((m == MODE_GATE || m == MODE_TRIG || m == MODE_PULSE) && !armed) {
    return "ARM1_REQUIRED";
  }
  if (m == MODE_PULSE && armed) {
    return pulse_ready ? "PULSE_PRIMED" : "PULSE_NOT_PRIMED";
  }
  return nullptr;
}

static void print_status_line() {
  Serial.print("STATE=");
  Serial.print(state_str((State)state_now));

  Serial.print(" PER=");
  Serial.print((unsigned long)per_us);

  Serial.print(" WID=");
  Serial.print((unsigned long)wid_us);

  Serial.print(" MODE=");
  Serial.print(mode_str((UserMode)user_mode));

  Serial.print(" ARM=");
  Serial.print(armed ? 1 : 0);

  Serial.print(" TRIG=");
  Serial.print(trig_level ? 1 : 0);

  Serial.print(" OUT=");
  Serial.print((pwm_active || pulse_now_active) ? 1 : 0);

  Serial.print(" PRDY=");
  Serial.print(pulse_ready ? 1 : 0);

  const char* hint = status_hint();
  if (hint) {
    Serial.print(" HINT=");
    Serial.print(hint);
  }

  Serial.println();
}

static void print_help_brief() {
  // Minimal one-line synopsis suitable for startup banner.
  Serial.println("HELP: per <us> | wid <us> | mode none|loop|gate|trig|pulse | arm 1|0 | trig(trigger) | pulse | stop(q) | status | ping | led on/off");
}

static void print_help_full() {
  // Short but sticky reference intended to be readable from any terminal.
  Serial.println();
  Serial.println("PulseGen v0.8 (UNO R4)  Serial 115200 NL");
  Serial.println();
  Serial.println("Quick use:");
  Serial.println("  status                 show state");
  Serial.println("  per <us>   wid <us>     set params (PWM: wid < per)");
  Serial.println("  mode <none|loop|gate|trig|pulse>");
  Serial.println("  arm <0|1>               enable trigger processing");
  Serial.println("  trig                    software rising edge (arm=1)");
  Serial.println("  pulse                   immediate one-shot now (any mode)");
  Serial.println("  stop | q                output LOW, mode NONE, arm 0");
  Serial.println();
  Serial.println("Modes:");
  Serial.println("  none : output off");
  Serial.println("  loop : PWM free-run (arm=0) OR start on next rising edge (arm=1)");
  Serial.println("  gate : PWM only while D2=HIGH (requires arm=1)");
  Serial.println("  trig : 1 pulse per rising edge on D2, width=WID (requires arm=1, PER ignored)");
  Serial.println("  pulse: only NEXT rising edge fires 1 pulse (requires arm=1), then ignores");
  Serial.println();
  Serial.println("Pins:");
  Serial.println("  OUT=D9   TRIG=D2 (INPUT_PULLDOWN)");
  Serial.println();
  Serial.println("Notes:");
  Serial.println("  - PWM modes are the precision timing modes.");
  Serial.println("  - One-shot pulses have ~10us practical floor even if WID smaller.");
  Serial.println();
}

// -------------------- Trigger ISR --------------------
void onTriggerChange() {
  // Tracks level and edge events. Rising edge is the event used for LOOP start, TRIG, and PULSE.
  bool lvl = (digitalRead(TRIG_PIN) != 0);
  trig_level = lvl;
  if (lvl) trig_rise_pending = true;
  else     trig_fall_pending = true;
}

// -------------------- Panic stop --------------------
static void stop_all_to_idle() {
  // Brings the system to a quiet, deterministic state.
  soft_trigger_pending = false;
  trig_rise_pending = false;
  trig_fall_pending = false;

  pwm_update_pending = false;

  pwm_stop_force_low_foreground();

  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, LOW);
  pulse_now_active = false;
  restore_pwm_after_pulse_now = false;

  state_now = STATE_IDLE;
}

// -------------------- Command handler --------------------
static inline void reply_ok()  { Serial.println("OK"); }
static inline void reply_err() { Serial.println("ERR"); }

static void set_mode(UserMode m) {
  user_mode = m;

  // MODE_PULSE is a one-shot-on-next-edge latch.
  if (user_mode == MODE_PULSE) pulse_ready = true;
  else pulse_ready = false;

  // Immediate, mode-correct normalization + warnings.
  normalize_params_for_mode((UserMode)user_mode, true);

  apply_selection_foreground();
}

static bool is_help_alias(const String& s) {
  // Help aliases intended for terminal muscle memory.
  return equals_ci(s, "help") || equals_ci(s, "-h") || equals_ci(s, "--help") || equals_ci(s, "?") || equals_ci(s, "/?");
}

static void handle_line(String s_raw) {
  String s = normalize_command(s_raw);
  if (s.length() == 0) return;

  if (equals_ci(s, "ping"))   { reply_ok(); return; }
  if (equals_ci(s, "status")) { print_status_line(); return; }

  if (is_help_alias(s)) {
    // Full help is more useful than the one-line synopsis when explicitly requested.
    print_help_full();
    return;
  }

  if (equals_ci(s, "stop") || equals_ci(s, "q")) {
    armed = false;
    user_mode = MODE_NONE;
    pulse_ready = false;
    stop_all_to_idle();
    reply_ok();
    print_status_line();
    return;
  }

  if (equals_ci(s, "led on"))  { digitalWrite(LED_BUILTIN, HIGH); reply_ok(); return; }
  if (equals_ci(s, "led off")) { digitalWrite(LED_BUILTIN, LOW);  reply_ok(); return; }

  if (starts_with_ci(s, "per ")) {
    long v = to_int_from(s, 4);
    if (v <= 0) { reply_err(); return; }
    per_us = (uint32_t)v;

    normalize_params_for_mode((UserMode)user_mode, true);

    Serial.print("OK PER=");
    Serial.println((unsigned long)per_us);

    if (mode_is_pwm((UserMode)user_mode) && pwm_active && !pulse_now_active) {
      pwm_update_pending = true;
    }

    print_status_line();
    return;
  }

  if (starts_with_ci(s, "wid ")) {
    long v = to_int_from(s, 4);
    if (v <= 0) { reply_err(); return; }
    wid_us = (uint32_t)v;

    normalize_params_for_mode((UserMode)user_mode, true);

    Serial.print("OK WID=");
    Serial.println((unsigned long)wid_us);

    if (mode_is_pwm((UserMode)user_mode) && pwm_active && !pulse_now_active) {
      pwm_update_pending = true;
    }

    print_status_line();
    return;
  }

  if (starts_with_ci(s, "mode ") || starts_with_ci(s, "mod ")) {
    int start = starts_with_ci(s, "mode ") ? 5 : 4;
    String arg = s.substring(start);
    arg.trim();

    if (equals_ci(arg, "none"))  { set_mode(MODE_NONE);  Serial.println("OK MODE=NONE");  print_status_line(); return; }
    if (equals_ci(arg, "loop"))  { set_mode(MODE_LOOP);  Serial.println("OK MODE=LOOP");  print_status_line(); return; }
    if (equals_ci(arg, "gate") || equals_ci(arg, "gated")) { set_mode(MODE_GATE); Serial.println("OK MODE=GATE"); print_status_line(); return; }
    if (equals_ci(arg, "trig") || equals_ci(arg, "trigger")) { set_mode(MODE_TRIG); Serial.println("OK MODE=TRIG"); print_status_line(); return; }
    if (equals_ci(arg, "pulse")) { set_mode(MODE_PULSE); Serial.println("OK MODE=PULSE"); print_status_line(); return; }

    reply_err();
    return;
  }

  if (starts_with_ci(s, "arm ")) {
    long v = to_int_from(s, 4);
    if (v != 0 && v != 1) { reply_err(); return; }

    armed = (v == 1);

    // Clear any latched edge flags so an old edge does not fire after an arm change.
    soft_trigger_pending = false;
    trig_rise_pending = false;
    trig_fall_pending = false;

    // Re-prime PULSE mode when arming; disarm clears it.
    if (armed && user_mode == MODE_PULSE) pulse_ready = true;
    if (!armed) pulse_ready = false;

    normalize_params_for_mode((UserMode)user_mode, true);
    apply_selection_foreground();

    reply_ok();
    print_status_line();
    return;
  }

  if (equals_ci(s, "trig") || equals_ci(s, "trigger")) {
    // Software-injected rising edge for testing without wiring D2.
    if (!armed) { reply_err(); return; }
    soft_trigger_pending = true;
    reply_ok();
    return;
  }

  if (equals_ci(s, "pulse")) {
    // Immediate pulse now, independent of the selected mode.
    if (!pulse_now_active) {
      start_pulse_now_user_wid((uint32_t)wid_us, true);
      state_now = STATE_PULSE_ACTIVE;
    }
    reply_ok();
    return;
  }

  // Convenience: accept "help" even if user types "h" or "-?" in the future (optional expansion)
  // reply_err() remains the safe default for unknown commands.
  reply_err();
}

// -------------------- Arduino setup/loop --------------------
void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  // Ensure output starts low and quiet.
  pinMode(OUT_PIN, OUTPUT);
  digitalWrite(OUT_PIN, LOW);

  // Trigger input defaults to low unless externally driven high.
  pinMode(TRIG_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(TRIG_PIN), onTriggerChange, CHANGE);

  trig_level = (digitalRead(TRIG_PIN) != 0);

  Serial.begin(115200);
  while (!Serial) { delay(10); }

  if (!init_width_timer()) Serial.println("ERR:TIMER");

  // Defaults are PWM-friendly while MODE=NONE.
  normalize_params_for_mode((UserMode)user_mode, false);

  delay(200);
  Serial.println("READY");
  print_help_brief();
  print_status_line();
}

void loop() {
  // ----- Serial input -----
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;

    if (c == '\n') {
      handle_line(line);
      line = "";
    } else {
      if (line.length() < 160) line += c;
    }
  }

  // If a pulse finished and we had detached PWM, restore selection.
  if (!pulse_now_active && restore_pwm_after_pulse_now) {
    restore_pwm_after_pulse_now = false;
    apply_selection_foreground();
  }

  // If pulse-active state ended, re-apply selection.
  if (state_now == STATE_PULSE_ACTIVE && !pulse_now_active) {
    apply_selection_foreground();
  }

  // Apply pending PWM changes
  if (pwm_update_pending && !pulse_now_active) {
    pwm_update_pending = false;
    if (mode_is_pwm((UserMode)user_mode) && (user_mode == MODE_LOOP || user_mode == MODE_GATE)) {
      if (pwm_active) {
        normalize_params_for_mode((UserMode)user_mode, false);
        pwm_start_or_restart_foreground((uint32_t)per_us, (uint32_t)wid_us);
      }
    }
  }

  // Snapshot and clear trigger events
  bool do_soft_rise = soft_trigger_pending;
  if (do_soft_rise) soft_trigger_pending = false;

  bool do_rise = trig_rise_pending;
  bool do_fall = trig_fall_pending;
  if (do_rise) trig_rise_pending = false;
  if (do_fall) trig_fall_pending = false;

  if (do_soft_rise) do_rise = true;

  // ----- GATE: PWM while TRIG high -----
  if (user_mode == MODE_GATE) {
    if (!armed || pulse_now_active) {
      if (pwm_active) pwm_stop_force_low_foreground();
      state_now = STATE_IDLE;
    } else {
      bool lvl = trig_level;
      if (lvl) {
        if (!pwm_active) {
          normalize_params_for_mode((UserMode)user_mode, false);
          pwm_start_or_restart_foreground((uint32_t)per_us, (uint32_t)wid_us);
        }
        state_now = pwm_active ? STATE_GATE_RUNNING : STATE_IDLE;
      } else {
        if (pwm_active) pwm_stop_force_low_foreground();
        state_now = STATE_IDLE;
      }
    }
    return;
  }

  // ----- LOOP -----
  if (user_mode == MODE_LOOP) {
    if (pulse_now_active) return;

    if (!armed) {
      if (!pwm_active) {
        normalize_params_for_mode((UserMode)user_mode, false);
        pwm_start_or_restart_foreground((uint32_t)per_us, (uint32_t)wid_us);
      }
      state_now = pwm_active ? STATE_LOOP_RUNNING : STATE_IDLE;
    } else {
      // Armed loop starts on a rising edge; once started it runs continuously.
      if (do_rise && !pwm_active) {
        normalize_params_for_mode((UserMode)user_mode, false);
        pwm_start_or_restart_foreground((uint32_t)per_us, (uint32_t)wid_us);
      }
      state_now = pwm_active ? STATE_LOOP_RUNNING : STATE_IDLE;
    }
    return;
  }

  // ----- TRIG / PULSE: single pulse on rising edge (GPIO + timer) -----
  if (user_mode == MODE_TRIG || user_mode == MODE_PULSE) {
    if (pwm_active) pwm_stop_force_low_foreground();
    if (!armed || pulse_now_active) return;
    if (!do_rise) return;

    if (user_mode == MODE_PULSE) {
      // Only one rising edge is allowed while primed.
      if (!pulse_ready) return;
      pulse_ready = false;
    }

    if (!pulse_now_active) {
      start_pulse_now_user_wid((uint32_t)wid_us, false);
      state_now = STATE_PULSE_ACTIVE;
    }
    return;
  }

  // ----- NONE -----
  if (user_mode == MODE_NONE) {
    if (pwm_active) pwm_stop_force_low_foreground();
    if (!pulse_now_active) state_now = STATE_IDLE;
    return;
  }
}