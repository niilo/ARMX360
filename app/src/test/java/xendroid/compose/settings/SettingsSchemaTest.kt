package xendroid.compose.settings

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import xendroid.compose.settings.Setting
import xendroid.compose.settings.SettingsSchema
import java.io.File

/** Schema-integrity checks (no emulator / JNI needed). */
class SettingsSchemaTest {

    private val all = SettingsSchema.allSettings

    // 101 Bool + 12 IntRange + 21 ListChoice + 2 Action = 136. Display|host_present_from_non_ui_thread
    // is intentionally absent (forced true natively; not a valid user choice).
    @Test fun total_entry_count_is_136() {
        assertEquals(136, all.size)
        assertEquals(
            136,
            all.count { it is Setting.Bool } + all.count { it is Setting.IntRange } +
                all.count { it is Setting.ListChoice } + all.count { it is Setting.Action },
        )
    }

    @Test fun counts_by_type_match_verified_inventory() {
        assertEquals(101, all.count { it is Setting.Bool })
        assertEquals(12, all.count { it is Setting.IntRange })
        assertEquals(21, all.count { it is Setting.ListChoice })
        assertEquals(2, all.count { it is Setting.Action })
    }

    /** These keys are looked up by string with a hard cast, so a section move that changes
     *  the key must not go unnoticed. */
    @Test fun keys_referenced_by_code_resolve_to_the_right_type() {
        listOf("Console|user_language", "Console|user_country").forEach { key ->
            val s = SettingsSchema.byKey[key]
            assertNotNull("missing schema key referenced in code: $key", s)
            assertTrue(
                "$key must be a ListChoice for the profile screens",
                s is Setting.ListChoice,
            )
        }
    }

    @Test fun keys_are_unique() {
        assertEquals(all.size, SettingsSchema.byKey.size)
        assertEquals(all.size, all.map { it.key }.toSet().size)
    }

    @Test fun categories_present_in_legacy_order() {
        val expected = listOf(
            "Vulkan", "Video", "UI", "Storage", "Kernel", "Controller", "HID", "Memory", "XConfig",
            "Display", "GPU", "CPU", "Logging", "Content", "General", "APU",
        )
        assertEquals(expected, SettingsSchema.categories.map { it.title })
    }

    @Test fun removed_no_op_settings_stay_removed() {
        assertNull(SettingsSchema.byKey["Kernel|Allow_nui_initialization"])
    }

    @Test fun actions_are_the_driver_picker_and_the_log_export() {
        val actions = all.filterIsInstance<Setting.Action>().map { it.key }
        assertEquals(listOf("Vulkan|vulkan_lib_path", "Logging|dump_session_logs"), actions)
    }

    @Test fun list_defaults_are_empty_or_a_member_of_options() {
        all.filterIsInstance<Setting.ListChoice>().forEach { lc ->
            if (lc.default.isNotEmpty()) {
                assertTrue(
                    "ListChoice ${lc.key} default '${lc.default}' must resolve to an option",
                    lc.options.any { it.value == lc.default },
                )
            }
        }
    }

    @Test fun user_language_skips_10_and_maps_8_and_17_to_zh() {
        val lc = SettingsSchema.byKey["Console|user_language"] as Setting.ListChoice
        assertTrue(lc.options.none { it.value == "10" })
        assertEquals("zh", lc.options.first { it.value == "8" }.label)
        assertEquals("zh", lc.options.first { it.value == "17" }.label)
    }

    @Test fun user_country_has_107_options_skips_17_and_94_and_default_103_resolves() {
        val lc = SettingsSchema.byKey["Console|user_country"] as Setting.ListChoice
        assertEquals(107, lc.options.size)
        assertTrue(lc.options.none { it.value == "17" })
        assertTrue(lc.options.none { it.value == "94" })
        assertNotNull(lc.options.firstOrNull { it.value == "103" })
        assertEquals("103", lc.default)
    }

    @Test fun int_ranges_match_verified_xml() {
        fun ir(key: String) = SettingsSchema.byKey[key] as Setting.IntRange
        ir("Memory|mmap_address_high").let {
            assertEquals(2, it.min); assertEquals(63, it.max); assertEquals(8, it.default)
        }
        ir("GPU|texture_cache_memory_limit_soft").let {
            // min == the real TOML default (384); a higher floor would silently coerce the
            // default upward.
            assertEquals(384, it.min); assertEquals(4096, it.max); assertEquals(384, it.default)
        }
        ir("GPU|texture_cache_memory_limit_hard").let {
            assertEquals(512, it.min); assertEquals(4096, it.max); assertEquals(768, it.default)
        }
        ir("General|time_scalar").let {
            assertEquals(1, it.min); assertEquals(8, it.max)
        }
        ir("Console|xmp_default_volume").let {
            assertEquals(0, it.min); assertEquals(100, it.max)
        }
        ir("APU|apu_max_queued_frames").let {
            assertEquals(4, it.min); assertEquals(64, it.max)
        }
    }

    /** Every IntRange default must be in [min, max], else the slider silently coerces the
     *  persisted default to a different value (the texture-cache bug). */
    @Test fun int_range_defaults_within_bounds() {
        SettingsSchema.allSettings.filterIsInstance<Setting.IntRange>().forEach {
            assert(it.default in it.min..it.max) {
                "${it.key}: default ${it.default} outside [${it.min}, ${it.max}]"
            }
            assert(it.min <= it.max) { "${it.key}: min ${it.min} > max ${it.max}" }
        }
    }

    // ---- Kotlin UI default vs. the default the native binary actually runs ----
    //
    // The settings screen renders schema defaults when a key is absent from the live config,
    // and SettingsRepository.isModified() compares the live value against them too. So when
    // the schema default disagrees with the effective native default, the UI shows a toggle
    // in a state the emulator is not in, and a "modified" badge that is simply wrong.
    //
    // Effective native default = the bundled template's value if the template ships the key,
    // else the hardcoded DEFINE_bool default. (The template is copied to
    // xenia-canary.config.toml on first run, so a shipped key always wins; a key the template
    // omits falls through to the compiled-in DEFINE_bool.) vulkan_in_pass_resolve was exactly
    // this bug: template omits it, DEFINE_bool says false, schema said true.
    //
    // Same spirit as encoding the A/B traps as machine-checked preconditions in
    // tools/bench-ab.sh rather than trusting memory: a divergence here is invisible in review
    // and misleads every user of the settings screen, so it is asserted, not documented.

    private fun repoRoot(): File {
        var dir = File(System.getProperty("user.dir")).absoluteFile
        while (true) {
            if (File(dir, "settings.gradle").isFile &&
                File(dir, "emulator-core/src/main/cpp").isDirectory
            ) return dir
            dir = dir.parentFile ?: error(
                "could not locate the ARMX360 repo root from ${System.getProperty("user.dir")}"
            )
        }
    }

    /** `name = true|false` at the start of a line, ignoring the trailing `# comment`. */
    private fun boolLiterals(text: String): Map<String, Boolean> =
        Regex("""(?m)^\s*([a-z0-9_]+)\s*=\s*(true|false)\b""")
            .findAll(text)
            .associate { it.groupValues[1] to (it.groupValues[2] == "true") }

    /** `DEFINE_bool(name, default, "help"...` — the default is always the 2nd argument.
     *  Scans .cc and .cpp: the app's own entry points (xendroid_emu.cpp) define their share of
     *  the cvars this screen exposes. */
    private fun nativeBoolDefaults(root: File): Map<String, Boolean> {
        val out = HashMap<String, Boolean>()
        val cppDir = File(root, "emulator-core/src/main/cpp")
        val re = Regex("""DEFINE_bool\(\s*([A-Za-z0-9_]+)\s*,\s*(true|false)\s*,""")
        cppDir.walkTopDown()
            .filter { it.isFile && (it.extension == "cc" || it.extension == "cpp") }
            .forEach { f ->
                re.findAll(f.readText()).forEach { m ->
                    // First definition wins: a debug_* probe cvar shadowing a real one is noise.
                    out.putIfAbsent(m.groupValues[1], m.groupValues[2] == "true")
                }
            }
        return out
    }

    @Test fun bool_defaults_match_effective_native_default() {
        val root = repoRoot()
        val template = boolLiterals(
            File(root, "emulator-core/src/main/assets/config/default_config.toml").readText()
        )
        val native = nativeBoolDefaults(root)

        val problems = ArrayList<String>()
        var compared = 0
        for (s in all.filterIsInstance<Setting.Bool>()) {
            // Keys with no DEFINE_bool anywhere are not native cvars (app-level switches like
            // `mute`, `readback_memexport`); they legitimately have no native default to match.
            val nativeDefault = native[s.name] ?: continue
            val effective = template[s.name] ?: nativeDefault
            compared++
            if (effective != s.default) {
                val from = if (s.name in template) "bundled template" else "DEFINE_bool"
                problems += "${s.key}: ui=${s.default} but effective=$effective (from $from)"
            }
        }

        assertTrue(
            "compared only $compared Bools - the native scan found far fewer than expected, " +
                "so the check is not actually running",
            compared >= 90,
        )
        assertEquals(
            "Kotlin schema defaults disagree with the effective native default(s):\n" +
                problems.joinToString("\n") +
                "\nThe settings screen renders these defaults when a key is absent from the " +
                "live config, so each one shows the user a state the emulator is not in.",
            0,
            problems.size,
        )
    }

    /** The specific regression, pinned so the general check above cannot mask it. A key the
     *  bundled template deliberately omits must still match its compiled-in default. */
    @Test fun vulkan_in_pass_resolve_default_is_false() {
        val s = SettingsSchema.byKey["Vulkan|vulkan_in_pass_resolve"] as Setting.Bool
        assertFalse(
            "vulkan_in_pass_resolve is omitted from default_config.toml, so DEFINE_bool's " +
                "false (vulkan_render_target_cache.cc) is what a stock install runs",
            s.default,
        )
    }
