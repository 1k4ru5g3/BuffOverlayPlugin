// ============================================================================
// ExampleSkills — Skill Bar Live View + Animation Timing Sampler + DAT Inspector
// ============================================================================
// Five blocks demonstrating the v6 SDK Active Skill surface:
//   0. Bar Slots→DPS  — the headline use case: for each visible skill-bar slot,
//                       resolve the skill via IsOnSkillBar/SkillSlotIndex and
//                       read its game-computed DPS to overlay on that icon.
//   1. Live View      — full per-frame table of every active skill
//   2. Timing Sampler — empirically measures animation duration by sampling
//                       Actor.AnimationId transitions, invalidates on buff
//                       changes. The recommended pattern for combat-macro
//                       plugins.
//   3. DAT Inspector  — hex-dumps the 0x100-byte ActiveSkillDetails struct
//                       via Memory.Read so authors can reverse new fields
//                       without touching the SDK.
//   4. Stat Sets/DPS  — EnumerateSkillStats: the game's evaluated per-skill
//                       stat containers ({stat_id, value} pairs, stat_id =
//                       Stats.dat row + 1) + how to read the DPS family
//                       (691 aps*100 / 692 dps*100 / 695 cps*100 — VIRTUAL
//                       stats, present only where the game evaluated them).
// ============================================================================
#pragma once

#include "../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../../../POEFixer/imgui/imgui.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Examples {

struct SkillTiming {
    int      anim_id_during_use = -1;
    float    last_measured_ms = 0.0f;
    float    ema_ms = 0.0f;
    int      samples = 0;
    enum class Mode { None, Auto, Manual } mode = Mode::None;
    int32_t  last_cooldown_seen = 0; // for auto attribution (delta detect)
};

struct SkillsTabState {
    std::unordered_map<std::string, SkillTiming> timings;  // keyed by skill.Name
    std::string  manual_calibrating_skill;                 // empty if no Calibrate primed
    int          last_anim_id = 0;                          // previous frame's Actor.AnimationId
    bool         last_anim_was_action = false;              // not Idle/Run/Death/TakeHit
    double       transition_t_ms = 0.0;                     // when current action animation began
    uint64_t     buffs_fingerprint = 0;
    uint64_t     prev_buffs_fingerprint = 0;
    int          dat_inspect_skill_index = -1;              // for DAT Inspector
    int          stat_sets_skill_index = -1;                // for Stat Sets / DPS block

    // Generic Idle→non-Idle→Idle cycle tracker (latches the most recent
    // non-Idle duration; shown next to "Current Anim:" once the cycle ends).
    // Independent from the per-skill `timings` map above — those use Calibrate
    // / cooldown-delta attribution, while this one is a raw single-shot timer.
    int         auto_prev_anim_id      = -1;
    std::string auto_prev_anim_name;        // name of the animation observed last frame (for attribution)
    bool        auto_measuring         = false;
    double      auto_start_ms          = 0.0;
    float       auto_last_duration_ms  = 0.0f;
    std::string auto_last_anim_name;        // name of the most recent measured animation
};

inline double NowMs() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

// Whitelist of buff name fragments that affect skill animation timing.
// Extend freely — the fingerprint just needs to flip when a relevant buff
// comes/goes/changes charge count.
inline bool IsSpeedBuff(std::string_view name) {
    static const char* kFragments[] = {
        "attack_speed", "cast_speed", "frenzy", "onslaught",
        "haste", "swiftness", "tailwind", "berserk",
        "adrenaline", "phasing", "speed_aura",
    };
    for (const char* f : kFragments) {
        if (name.find(f) != std::string_view::npos) return true;
    }
    return false;
}

inline uint64_t HashBuffsFingerprint(const std::vector<PluginSDK::Buff>& buffs) {
    // FNV-1a over (name + charges) of speed-affecting buffs only.
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            h ^= p[i];
            h *= 0x100000001b3ull;
        }
    };
    for (const auto& b : buffs) {
        if (!IsSpeedBuff(b.Name)) continue;
        mix(b.Name.data(), b.Name.size());
        int16_t ch = b.Charges;
        mix(&ch, sizeof(ch));
    }
    return h;
}

// "Action" animations indicate the player is mid-skill. Idle/Run/Death/TakeHit
// are explicitly NOT actions. The Animation enum is large (~1000+ entries),
// so we use a small denylist instead of a whitelist.
inline bool IsActionAnimation(int anim_id) {
    switch (anim_id) {
        case 0x0:  // Idle
        case 0x4:  // Run
        case 0x5:  // TakeHit
        case 0x6:  // Death
            return false;
        default:
            return true;
    }
}

inline void DrawSkillsPanel(const PluginSDK::Context* ctx,
                            const PluginSDK::Snapshot& snapshot,
                            SkillsTabState& state) {
    if (!snapshot.Player.Components.HasActor()) {
        ImGui::TextColored(ImVec4(1, 0.6f, 0.2f, 1), "Player has no Actor component");
        return;
    }
    auto actor  = ctx->Components.ReadActor(snapshot.Player.Components.Actor);
    auto skills = ctx->Components.EnumerateActiveSkills(snapshot.Player.Components.Actor);
    auto buffs  = snapshot.Player.Components.HasBuffs()
                      ? ctx->Components.EnumerateBuffs(snapshot.Player.Components.Buffs)
                      : std::vector<PluginSDK::Buff>{};

    // Update buffs fingerprint, invalidate timings on change.
    state.prev_buffs_fingerprint = state.buffs_fingerprint;
    state.buffs_fingerprint = HashBuffsFingerprint(buffs);
    if (state.buffs_fingerprint != state.prev_buffs_fingerprint) {
        for (auto& kv : state.timings) {
            kv.second.samples = 0;
            kv.second.ema_ms = 0.0f;
            kv.second.last_measured_ms = 0.0f;
        }
    }

    // --- Block 0: Skill Bar Slots -> DPS (headline use case) -------------
    // Each visible bar slot i is answered by the UNIQUE skill with
    // IsOnSkillBar && SkillSlotIndex == i (host-decoded from the skill's packed
    // id — see SkillBarDecode.h). This is exactly what a DPS-over-the-bar overlay
    // needs: walk slots 0..N, resolve the skill, read its game-computed DPS
    // (stat 692, EnumerateSkillStats) and draw it over that icon.
    if (ImGui::CollapsingHeader("Skill Bar Slots -> DPS", ImGuiTreeNodeFlags_DefaultOpen)) {
        int maxSlot = -1;
        for (const auto& s : skills)
            if (s.IsOnSkillBar && s.SkillSlotIndex > maxSlot) maxSlot = s.SkillSlotIndex;

        if (maxSlot < 0) {
            ImGui::TextDisabled("No skills detected on the main skill bar "
                                "(open the game with skills socketed).");
        } else if (ImGui::BeginTable("BarSlotsT", 3,
                                     ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 44.0f);
            ImGui::TableSetupColumn("Skill");
            ImGui::TableSetupColumn("DPS (game, stat 692)");
            ImGui::TableHeadersRow();
            for (int i = 0; i <= maxSlot; ++i) {
                const PluginSDK::ActiveSkill* found = nullptr;
                for (const auto& s : skills)
                    if (s.IsOnSkillBar && s.SkillSlotIndex == i) { found = &s; break; }

                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d", i);
                if (!found) {
                    ImGui::TableNextColumn(); ImGui::TextDisabled("(empty)");
                    ImGui::TableNextColumn(); ImGui::TextDisabled("-");
                    continue;
                }
                ImGui::TableNextColumn(); ImGui::Text("%s", found->Name.c_str());

                // Same-frame: `skills` was enumerated this frame, so
                // SkillDetailsAddr is fresh. Pull the game-computed DPS.
                int dps100 = -1;
                for (const auto& e : ctx->Components.EnumerateSkillStats(found->SkillDetailsAddr)) {
                    if (e.StatId == 692) { dps100 = e.Value; break; }
                }
                ImGui::TableNextColumn();
                if (dps100 >= 0) ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "%.2f", dps100 / 100.0);
                else             ImGui::TextDisabled("n/a (utility / avg-dmg skill)");
            }
            ImGui::EndTable();
        }
    }

    // --- Block 1: Live View ----------------------------------------------
    if (ImGui::CollapsingHeader("Skill Bar Live View", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::BeginTable("LiveSkillT", 8,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Bar");
            ImGui::TableSetupColumn("Stage");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Uses");
            ImGui::TableSetupColumn("CD(ms)");
            ImGui::TableSetupColumn("Charges");
            ImGui::TableSetupColumn("Usable");
            ImGui::TableHeadersRow();
            for (const auto& s : skills) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%s", s.Name.c_str());
                ImGui::TableNextColumn();
                if (s.IsOnSkillBar) ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1), "#%d", s.SkillSlotIndex);
                else                ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::Text("%d", s.UseStage);
                ImGui::TableNextColumn(); ImGui::Text("%d", s.CastType);
                ImGui::TableNextColumn(); ImGui::Text("%d", s.TotalUses);
                ImGui::TableNextColumn(); ImGui::Text("%d", s.TotalCooldownMs);
                ImGui::TableNextColumn();
                if (s.MaxUses > 0) {
                    int remaining = s.MaxUses - s.TotalActiveCooldowns;
                    if (remaining < 0) remaining = 0;
                    ImGui::Text("%d/%d", remaining, s.MaxUses);
                } else {
                    ImGui::TextDisabled("-/-");
                }
                ImGui::TableNextColumn();
                ImGui::TextColored(s.CanBeUsed ? ImVec4(0.3f,1,0.3f,1) : ImVec4(1,0.3f,0.3f,1),
                                   s.CanBeUsed ? "yes" : "no");
            }
            ImGui::EndTable();
        }
    }

    // --- Block 2: Animation Timing Sampler -------------------------------
    if (ImGui::CollapsingHeader("Animation Timing Sampler", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Buffs fingerprint: 0x%016llX  (%s)",
                    (unsigned long long)state.buffs_fingerprint,
                    state.buffs_fingerprint == state.prev_buffs_fingerprint ? "stable" : "just changed");

        // Detect animation transitions.
        const bool is_action = IsActionAnimation(actor.AnimationId);
        const bool was_action = state.last_anim_was_action;
        const double now_ms = NowMs();

        if (!was_action && is_action) {
            // Start of an action.
            state.transition_t_ms = now_ms;
        } else if (was_action && !is_action) {
            // End of an action — attribute to a skill if possible.
            const float dt = static_cast<float>(now_ms - state.transition_t_ms);

            // 1. Manual calibration always wins if armed.
            std::string attributed = state.manual_calibrating_skill;
            SkillTiming::Mode used_mode = SkillTiming::Mode::None;
            if (!attributed.empty()) {
                used_mode = SkillTiming::Mode::Manual;
                state.manual_calibrating_skill.clear();
            } else {
                // 2. Auto: skill whose total_active_cooldowns just incremented.
                for (const auto& s : skills) {
                    if (s.MaxUses <= 0) continue;
                    auto it = state.timings.find(s.Name);
                    int prev = (it != state.timings.end()) ? it->second.last_cooldown_seen : s.TotalActiveCooldowns;
                    if (s.TotalActiveCooldowns > prev) {
                        attributed = s.Name;
                        used_mode = SkillTiming::Mode::Auto;
                        break;
                    }
                }
            }

            if (!attributed.empty()) {
                auto& t = state.timings[attributed];
                t.last_measured_ms = dt;
                t.ema_ms = (t.samples == 0) ? dt : (0.7f * t.ema_ms + 0.3f * dt);
                t.samples += 1;
                t.mode = used_mode;
                t.anim_id_during_use = state.last_anim_id;
            }
        }

        // Update each skill's "last_cooldown_seen" so next-frame delta detection works.
        for (const auto& s : skills) {
            auto& t = state.timings[s.Name];
            t.last_cooldown_seen = s.TotalActiveCooldowns;
        }

        // Idle→non-Idle→Idle auto-timer (Idle id == 0). Latches duration AND
        // name on return to Idle. The previous-frame name is shadowed in
        // `auto_prev_anim_name` because `actor.AnimationName` at the moment
        // we detect the transition already refers to "Idle" — by then the
        // animation we wanted to attribute is gone from the live snapshot.
        {
            const int curId = actor.AnimationId;
            if (state.auto_prev_anim_id == -1) {
                state.auto_prev_anim_id = curId;
            } else if (state.auto_prev_anim_id == 0 && curId != 0) {
                state.auto_measuring = true;
                state.auto_start_ms  = now_ms;
            } else if (state.auto_prev_anim_id != 0 && curId == 0 && state.auto_measuring) {
                state.auto_last_duration_ms = static_cast<float>(now_ms - state.auto_start_ms);
                state.auto_last_anim_name   = state.auto_prev_anim_name;
                state.auto_measuring        = false;
            }
            state.auto_prev_anim_id   = curId;
            state.auto_prev_anim_name = actor.AnimationName;
        }

        if (state.auto_last_duration_ms > 0.0f) {
            ImGui::Text("Current Anim: %s (0x%X)%s — last: %.0f ms (%s)",
                        actor.AnimationName.c_str(), actor.AnimationId,
                        is_action ? " — action" : " — idle/run/etc",
                        state.auto_last_duration_ms,
                        state.auto_last_anim_name.c_str());
        } else {
            ImGui::Text("Current Anim: %s (0x%X)%s",
                        actor.AnimationName.c_str(), actor.AnimationId,
                        is_action ? " — action" : " — idle/run/etc");
        }
        ImGui::Spacing();

        if (ImGui::BeginTable("SamplerT", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Skill");
            ImGui::TableSetupColumn("Mode");
            ImGui::TableSetupColumn("Samples");
            ImGui::TableSetupColumn("Last(ms)");
            ImGui::TableSetupColumn("EMA(ms)");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (const auto& s : skills) {
                ImGui::TableNextRow();
                ImGui::PushID(s.Name.c_str());
                auto it = state.timings.find(s.Name);
                ImGui::TableNextColumn(); ImGui::Text("%s", s.Name.c_str());
                ImGui::TableNextColumn();
                if (it == state.timings.end() || it->second.mode == SkillTiming::Mode::None) {
                    ImGui::TextDisabled("-");
                } else {
                    ImGui::Text("%s", it->second.mode == SkillTiming::Mode::Auto ? "auto" : "manual");
                }
                ImGui::TableNextColumn(); ImGui::Text("%d", it != state.timings.end() ? it->second.samples : 0);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", it != state.timings.end() ? it->second.last_measured_ms : 0.0f);
                ImGui::TableNextColumn(); ImGui::Text("%.0f", it != state.timings.end() ? it->second.ema_ms : 0.0f);
                ImGui::TableNextColumn();
                if (!s.Name.empty() && state.manual_calibrating_skill == s.Name) {
                    if (ImGui::SmallButton("Cancel")) state.manual_calibrating_skill.clear();
                } else {
                    if (ImGui::SmallButton("Calibrate")) {
                        if (!s.Name.empty()) state.manual_calibrating_skill = s.Name;
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        if (ImGui::Button("Reset all timings")) {
            state.timings.clear();
        }

        ImGui::Spacing();
        ImGui::TextWrapped(
            "Tip: a macro-plugin spamming Whirling Slash + Twister can copy this "
            "Mode-Auto/Manual + EMA logic and use ema_ms as the inter-press delay. "
            "Auto attribution works for cooldown-bound skills (Twister); use "
            "[Calibrate] for non-cooldown skills (Whirling Slash) and replay a "
            "few hits to populate the EMA before relying on it.");
    }

    // --- Block 3: DAT Pointer Inspector ----------------------------------
    if (ImGui::CollapsingHeader("DAT Pointer Inspector")) {
        if (skills.empty()) {
            ImGui::TextDisabled("No active skills");
        } else {
            if (state.dat_inspect_skill_index < 0
                || state.dat_inspect_skill_index >= (int)skills.size()) {
                state.dat_inspect_skill_index = 0;
            }
            const char* current = skills[state.dat_inspect_skill_index].Name.c_str();
            if (ImGui::BeginCombo("Skill", current)) {
                for (int i = 0; i < (int)skills.size(); ++i) {
                    bool selected = (i == state.dat_inspect_skill_index);
                    if (ImGui::Selectable(skills[i].Name.c_str(), selected)) {
                        state.dat_inspect_skill_index = i;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            const auto& sel = skills[state.dat_inspect_skill_index];
            uintptr_t addr = sel.SkillDetailsAddr;
            ImGui::Text("SkillDetailsAddr: 0x%016llX", (unsigned long long)addr);
            uint8_t buf[0x100] = {};
            int32_t ok = 0;
            if (addr != 0) ok = ctx->Memory.Read(addr, buf, sizeof(buf));
            if (!ok) {
                ImGui::TextDisabled("Read failed or address null");
            } else {
                ImGui::BeginChild("DatHex", ImVec2(0, 260), true);
                for (size_t row = 0; row < sizeof(buf); row += 16) {
                    ImGui::Text("%04zX:", row);
                    ImGui::SameLine();
                    for (size_t col = 0; col < 16; ++col) {
                        ImGui::SameLine();
                        ImGui::Text("%02X", buf[row + col]);
                    }
                }
                ImGui::EndChild();
            }
        }
    }

    // --- Block 4: Evaluated Stat Sets + DPS -------------------------------
    // EnumerateSkillStats returns the game's own evaluated stat containers for
    // one skill: sorted {stat_id, value} pairs grouped by SetIndex. stat_id is
    // the Stats.dat ROW INDEX + 1 (the engine's runtime stat key). Set 0 is the
    // skill's CURRENT-CONTEXT set — always present, persists with panels
    // closed, and carries the DPS the in-game skills panel shows. Later sets
    // are per-part stat sets (summon/command skills — the minion damage lives
    // there). The DPS family (691 = attacks/s*100, 692 = DPS*100,
    // 695 = casts/s*100, 1982/1983 = avg damage *100) is computed by the game
    // for contexts it displays; alt contexts (infusion tabs, weapon swap) are
    // evaluated transiently and may not be present.
    if (ImGui::CollapsingHeader("Evaluated Stat Sets + DPS")) {
        if (skills.empty()) {
            ImGui::TextDisabled("No active skills");
        } else {
            if (state.stat_sets_skill_index < 0
                || state.stat_sets_skill_index >= (int)skills.size()) {
                state.stat_sets_skill_index = 0;
            }
            const char* current = skills[state.stat_sets_skill_index].Name.c_str();
            if (ImGui::BeginCombo("Skill##statsets", current)) {
                for (int i = 0; i < (int)skills.size(); ++i) {
                    bool selected = (i == state.stat_sets_skill_index);
                    if (ImGui::Selectable(skills[i].Name.c_str(), selected)) {
                        state.stat_sets_skill_index = i;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const auto& sel = skills[state.stat_sets_skill_index];
            // SkillDetailsAddr is same-frame fresh: `skills` was enumerated above.
            auto stats = ctx->Components.EnumerateSkillStats(sel.SkillDetailsAddr);

            // Derived DPS summary — scan every set for the DPS-family runtime ids.
            // A stat can appear in several sets (multi-part skills); take the
            // first occurrence, which is the game's primary evaluation.
            struct { int set = -1; int value = 0; } dps, aps, cps, avg_hit, avg_use, show_avg, cast_ms;
            for (const auto& e : stats) {
                auto take = [&](decltype(dps)& slot) {
                    if (slot.set < 0) { slot.set = e.SetIndex; slot.value = e.Value; }
                };
                switch (e.StatId) {
                    case 691:  take(aps);      break;  // hundred_times_attacks_per_second
                    case 692:  take(dps);      break;  // hundred_times_damage_per_second
                    case 695:  take(cps);      break;  // hundred_times_casts_per_second
                    case 1982: take(avg_hit);  break;  // hundred_times_average_damage_per_hit
                    case 1983: take(avg_use);  break;  // hundred_times_average_damage_per_skill_use
                    case 2079: take(show_avg); break;  // skill_show_average_damage_instead_of_dps
                    case 694:  take(cast_ms);  break;  // base_spell_cast_time_ms
                }
            }

            if (dps.set >= 0) {
                ImGui::TextColored(ImVec4(0.3f, 1, 0.3f, 1),
                                   "Game-computed DPS: %.2f  (stat 692, set %d)",
                                   dps.value / 100.0, dps.set);
            } else if (show_avg.set >= 0 && show_avg.value != 0) {
                ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1),
                                   "This skill displays AVERAGE DAMAGE instead of DPS (stat 2079 set)");
            } else {
                ImGui::TextDisabled(
                    "No DPS stat in this skill's evaluated sets (utility/support "
                    "skill, or average-damage display mode).");
            }
            if (aps.set >= 0)     ImGui::Text("Attacks/s: %.2f", aps.value / 100.0);
            if (cps.set >= 0)     ImGui::Text("Casts/s: %.2f", cps.value / 100.0);
            if (avg_hit.set >= 0) ImGui::Text("Avg damage per hit: %.2f", avg_hit.value / 100.0);
            if (avg_use.set >= 0) ImGui::Text("Avg damage per use: %.2f", avg_use.value / 100.0);
            if (cast_ms.set >= 0) ImGui::Text("Base cast time: %d ms", cast_ms.value);
            // Cross-check the game's formula: dps = rate/100 * avg (both x100).
            if (dps.set >= 0 && avg_use.set >= 0 && (aps.set >= 0 || cps.set >= 0)) {
                double rate = (aps.set >= 0 ? aps.value : cps.value) / 100.0;
                ImGui::TextDisabled("Cross-check rate*avg = %.2f", rate * (avg_use.value / 100.0));
            }
            ImGui::Spacing();

            if (stats.empty()) {
                ImGui::TextDisabled("No stat sets resolved for this skill");
            } else {
                int currentSet = -1;
                bool setOpen = false;
                for (const auto& e : stats) {
                    if (e.SetIndex != currentSet) {
                        if (setOpen) { ImGui::EndTable(); ImGui::TreePop(); }
                        currentSet = e.SetIndex;
                        setOpen = false;
                        int count = 0;
                        for (const auto& e2 : stats) if (e2.SetIndex == currentSet) ++count;
                        char label[64];
                        std::snprintf(label, sizeof(label), "Set %d (%d stats)", currentSet, count);
                        if (ImGui::TreeNode(label)) {
                            if (ImGui::BeginTable("SkillStatT", 3,
                                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
                                ImGui::TableSetupColumn("StatId (rid+1)", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                                ImGui::TableSetupColumn("Value/100");
                                ImGui::TableHeadersRow();
                                setOpen = true;
                            } else {
                                ImGui::TreePop();
                            }
                        }
                    }
                    if (setOpen) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); ImGui::Text("%d", e.StatId);
                        ImGui::TableNextColumn(); ImGui::Text("%d", e.Value);
                        ImGui::TableNextColumn(); ImGui::Text("%.2f", e.Value / 100.0);
                    }
                }
                if (setOpen) { ImGui::EndTable(); ImGui::TreePop(); }
                ImGui::TextWrapped(
                    "StatId is the Stats.dat row index + 1 (0 = the game's \"no "
                    "stat\" sentinel). Resolve names by dumping Stats.dat: row "
                    "690 = hundred_times_attacks_per_second -> runtime id 691, "
                    "etc. Sets mirror the skill's GrantedEffectStatSets parts "
                    "plus the merged character-context table (the big one).");
            }
        }
    }

    state.last_anim_id = actor.AnimationId;
    state.last_anim_was_action = IsActionAnimation(actor.AnimationId);
}

}  // namespace Examples
