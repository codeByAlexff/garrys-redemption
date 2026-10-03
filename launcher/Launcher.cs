// Garry's Redemption Passthrough: the launcher.
//
// One window that finds both games, says what is missing, installs the mod's files, offers
// to set the one NVIDIA driver setting the overlay needs, and starts everything in the right
// order. It sits in the release folder next to RDR2\ and GarrysMod\ (tools/package.ps1) and
// installs from there.
//
// Plain C# 5 and Windows Forms on purpose: it is compiled by the csc.exe that every Windows
// has with .NET Framework 4 (launcher/build.ps1), so a release needs no runtime or installer.
// It never touches Red Dead Online, and it ships nothing of either game.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using Microsoft.Win32;

namespace GarrysRedemption
{
    // ---- NVIDIA's "Vulkan/OpenGL present method" for RDR2.exe, through NVAPI's driver settings.
    //
    // Function ids are from nvapi_interface.h and the setting id from NvApiDriverSettings.h of
    // github.com/NVIDIA/nvapi (MIT); structure sizes, versions and offsets were printed by a
    // program compiled against that nvapi.h (2026-10-03): NVDRS_SETTING 12320 bytes, version
    // 0x00013020, settingId at 4100, settingType at 4104, current value at 8220;
    // NVDRS_APPLICATION 20492 bytes, version 0x0004500C. Buffers are filled by offset, so no
    // structure is declared here that could drift from the header.
    static class Nvidia
    {
        public const uint PresentMethodId = 0x20D690F8;  // OGL_CPL_PREFER_DXPRESENT_ID
        public const uint Native = 0;                    // PREFER_DISABLED
        public const uint LayeredOnDxgi = 1;             // PREFER_ENABLED
        public const uint Auto = 2;                      // AUTO, the driver's default

        const int SettingBytes = 12320, SettingVersion = 0x00013020;
        const int SettingIdAt = 4100, SettingTypeAt = 4104, SettingCurrentAt = 8220;
        const int AppBytes = 20492, AppVersion = 0x0004500C;
        const int SettingNotFound = -160;

        [DllImport("nvapi64.dll", EntryPoint = "nvapi_QueryInterface", CallingConvention = CallingConvention.Cdecl)]
        static extern IntPtr QueryInterface(uint id);

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int InitializeFn();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int CreateSessionFn(out IntPtr session);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SessionFn(IntPtr session);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int FindAppFn(IntPtr session, IntPtr name, out IntPtr profile, IntPtr app);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int GetSettingFn(IntPtr session, IntPtr profile, uint id, IntPtr setting);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int SetSettingFn(IntPtr session, IntPtr profile, IntPtr setting);

        static T Fn<T>(uint id) where T : class
        {
            IntPtr p = QueryInterface(id);
            if (p == IntPtr.Zero) throw new InvalidOperationException("NVAPI function " + id.ToString("X8") + " is missing");
            return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }

        public static bool Present()
        {
            return File.Exists(Path.Combine(Environment.SystemDirectory, "nvapi64.dll"));
        }

        // Reads (set == null) or writes the present method of the profile RDR2.exe belongs to.
        // Returns null on success, with `value` what the profile has (Auto if it has none);
        // otherwise what went wrong.
        public static string Access(uint? set, out uint value)
        {
            value = Auto;
            IntPtr session = IntPtr.Zero, name = IntPtr.Zero, app = IntPtr.Zero, setting = IntPtr.Zero;
            try
            {
                int status = Fn<InitializeFn>(0x0150E828)();
                if (status != 0) return "NVAPI did not start (" + status + ")";
                status = Fn<CreateSessionFn>(0x0694D52E)(out session);
                if (status != 0) return "no driver settings session (" + status + ")";
                status = Fn<SessionFn>(0x375DBD6B)(session);  // DRS_LoadSettings
                if (status != 0) return "the driver's settings could not be loaded (" + status + ")";

                name = Marshal.AllocHGlobal(4096);
                app = Marshal.AllocHGlobal(AppBytes);
                setting = Marshal.AllocHGlobal(SettingBytes);
                Zero(name, 4096);
                Zero(app, AppBytes);
                Zero(setting, SettingBytes);
                byte[] exe = Encoding.Unicode.GetBytes("rdr2.exe");
                Marshal.Copy(exe, 0, name, exe.Length);
                Marshal.WriteInt32(app, 0, AppVersion);
                IntPtr profile;
                status = Fn<FindAppFn>(0xEEE566B2)(session, name, out profile, app);
                if (status != 0) return "the driver has no profile for RDR2.exe (" + status + ")";

                if (set == null)
                {
                    Marshal.WriteInt32(setting, 0, SettingVersion);
                    status = Fn<GetSettingFn>(0x73BF8338)(session, profile, PresentMethodId, setting);
                    if (status == SettingNotFound) return null;  // not set in this profile: the default
                    if (status != 0) return "the setting could not be read (" + status + ")";
                    value = (uint)Marshal.ReadInt32(setting, SettingCurrentAt);
                    return null;
                }

                Marshal.WriteInt32(setting, 0, SettingVersion);
                Marshal.WriteInt32(setting, SettingIdAt, unchecked((int)PresentMethodId));
                Marshal.WriteInt32(setting, SettingTypeAt, 0);  // NVDRS_DWORD_TYPE
                Marshal.WriteInt32(setting, SettingCurrentAt, unchecked((int)set.Value));
                status = Fn<SetSettingFn>(0x577DD202)(session, profile, setting);
                if (status != 0) return "the driver refused the setting (" + status + ")";
                status = Fn<SessionFn>(0xFCBC7E14)(session);  // DRS_SaveSettings
                if (status != 0) return "the setting could not be saved (" + status + ")";
                value = set.Value;
                return null;
            }
            catch (Exception e)
            {
                return e.Message;
            }
            finally
            {
                if (session != IntPtr.Zero) { try { Fn<SessionFn>(0xDAD9CFF8)(session); } catch (Exception) { } }
                if (name != IntPtr.Zero) Marshal.FreeHGlobal(name);
                if (app != IntPtr.Zero) Marshal.FreeHGlobal(app);
                if (setting != IntPtr.Zero) Marshal.FreeHGlobal(setting);
            }
        }

        static void Zero(IntPtr p, int bytes)
        {
            for (int i = 0; i < bytes; i += 4) Marshal.WriteInt32(p, i, 0);
        }
    }

    // ---- where the games are
    static class Finder
    {
        public static bool IsRdr2(string dir)
        {
            return !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, "RDR2.exe"));
        }

        public static bool IsGmod(string dir)
        {
            return !string.IsNullOrEmpty(dir) && Directory.Exists(Path.Combine(dir, "garrysmod")) &&
                   (File.Exists(Path.Combine(dir, "hl2.exe")) || File.Exists(Path.Combine(dir, "gmod.exe")) || GmodExe(dir) != null);
        }

        // The 64-bit game: only the x86-64 branch has one.
        public static string GmodExe(string dir)
        {
            string[] names = { "gmod_win64.exe", @"bin\win64\gmod.exe" };
            foreach (string n in names)
            {
                string p = Path.Combine(dir, n);
                if (File.Exists(p)) return p;
            }
            return null;
        }

        static string Reg(RegistryKey root, string key, string value)
        {
            try
            {
                using (RegistryKey k = root.OpenSubKey(key))
                {
                    return k == null ? null : k.GetValue(value) as string;
                }
            }
            catch (Exception) { return null; }
        }

        static List<string> SteamLibraries()
        {
            List<string> libs = new List<string>();
            string steam = Reg(Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath");
            if (steam == null) steam = Reg(Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath");
            if (steam == null) return libs;
            steam = steam.Replace('/', '\\');
            libs.Add(steam);
            try
            {
                string vdf = File.ReadAllText(Path.Combine(steam, @"steamapps\libraryfolders.vdf"));
                foreach (Match m in Regex.Matches(vdf, "\"path\"\\s+\"([^\"]+)\""))
                {
                    string p = m.Groups[1].Value.Replace(@"\\", @"\");
                    if (!libs.Contains(p)) libs.Add(p);
                }
            }
            catch (Exception) { }
            return libs;
        }

        public static string FindGmod()
        {
            foreach (string lib in SteamLibraries())
            {
                string p = Path.Combine(lib, @"steamapps\common\GarrysMod");
                if (IsGmod(p)) return p;
            }
            return null;
        }

        public static string FindRdr2()
        {
            List<string> tries = new List<string>();
            tries.Add(Reg(Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Rockstar Games\Red Dead Redemption 2", "InstallFolder"));
            tries.Add(Reg(Registry.LocalMachine, @"SOFTWARE\Rockstar Games\Red Dead Redemption 2", "InstallFolder"));
            foreach (string lib in SteamLibraries()) tries.Add(Path.Combine(lib, @"steamapps\common\Red Dead Redemption 2"));
            try
            {
                string manifests = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                    @"Epic\EpicGamesLauncher\Data\Manifests");
                foreach (string item in Directory.GetFiles(manifests, "*.item"))
                {
                    string text = File.ReadAllText(item);
                    if (text.IndexOf("Red Dead Redemption 2", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    Match m = Regex.Match(text, "\"InstallLocation\"\\s*:\\s*\"([^\"]+)\"");
                    if (m.Success) tries.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
                }
            }
            catch (Exception) { }
            tries.Add(@"C:\Program Files\Rockstar Games\Red Dead Redemption 2");
            foreach (string t in tries)
            {
                if (t != null && IsRdr2(t.TrimEnd('\\'))) return t.TrimEnd('\\');
            }
            return null;
        }
    }

    enum Light { Good, Warn, Bad, Info }

    class Check
    {
        public Light Light;
        public string Title, Detail;
        public Check(Light light, string title, string detail) { Light = light; Title = title; Detail = detail; }
    }

    class MainForm : Form
    {
        const string Product = "Garry's Redemption Passthrough";
        const string GmodArgs = "-windowed -w 1920 -h 1080 -novid -condebug +maxplayers 1 +map gm_flatgrass";

        static readonly Color Back = Color.FromArgb(24, 19, 17), Panel = Color.FromArgb(36, 29, 26),
            Ink = Color.FromArgb(236, 226, 208), Dim = Color.FromArgb(160, 148, 130), Red = Color.FromArgb(196, 52, 40),
            Green = Color.FromArgb(108, 176, 92), Amber = Color.FromArgb(222, 168, 60);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern IntPtr FindWindow(string cls, string title);
        [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int w, int h, uint flags);
        [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
        [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);

        readonly TextBox rdr2Box = new TextBox(), gmodBox = new TextBox();
        readonly System.Windows.Forms.Panel checks = new System.Windows.Forms.Panel();
        readonly Label status = new Label();
        readonly Button play = new Button(), install = new Button(), nvidia = new Button(), stop = new Button();
        readonly CheckBox closeGmod = new CheckBox();
        readonly Timer timer = new Timer();
        readonly string payload = Path.GetDirectoryName(Application.ExecutablePath);
        readonly string settingsFile = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
            @"GarrysRedemption\launcher.txt");

        float scale = 1f;  // for what is laid out after the form's own scaling (the checklist)
        int S(int v) { return (int)(v * scale); }
        bool waitingToStartGmod, startedGmod, sawBoth, gmodTucked, needsInstall, canInstall;
        string nvidiaAdvice;

        public MainForm()
        {
            // Laid out for 96 dpi and scaled by hand to the display's, every position through S()
            // (seen at 150%: the text grew, the boxes did not, and every label was cut off; Windows
            // Forms' own scaling did nothing for controls placed in the constructor).
            AutoScaleMode = AutoScaleMode.None;
            using (Graphics g = Graphics.FromHwnd(IntPtr.Zero)) scale = g.DpiX / 96f;
            Text = Product;
            BackColor = Back;
            ForeColor = Ink;
            Font = new Font("Segoe UI", 9.5f);
            ClientSize = new Size(S(820), S(690));
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;

            Label title = MakeLabel("GARRY'S REDEMPTION", 24, 18, 600, 40, new Font("Segoe UI", 21f, FontStyle.Bold), Ink);
            MakeLabel("PASSTHROUGH", 27, 54, 300, 22, new Font("Segoe UI", 10.5f, FontStyle.Bold), Red);
            MakeLabel("Play Red Dead Redemption 2's story as a Garry's Mod player. Story mode only.", 27, 80, 760, 22, Font, Dim);
            title.BringToFront();

            PathRow("Red Dead Redemption 2", rdr2Box, 118, "The folder that holds RDR2.exe", Finder.IsRdr2);
            PathRow("Garry's Mod", gmodBox, 176, "The folder that holds the garrysmod folder", Finder.IsGmod);

            MakeLabel("CHECKLIST", 27, 240, 200, 18, new Font("Segoe UI", 8.5f, FontStyle.Bold), Dim);
            checks.SetBounds(S(24), S(262), S(772), S(290));
            checks.BackColor = Panel;
            checks.AutoScroll = true;
            Controls.Add(checks);

            StyleButton(install, "Install / update mod files", 24, 566, 200, 34, false);
            StyleButton(nvidia, "Fix the NVIDIA setting", 232, 566, 180, 34, false);
            StyleButton(stop, "Stop both games", 420, 566, 140, 34, false);
            Button logs = new Button();
            StyleButton(logs, "Logs", 568, 566, 70, 34, false);
            Button uninstall = new Button();
            StyleButton(uninstall, "Uninstall", 646, 566, 150, 34, false);
            StyleButton(play, "PLAY", 596, 614, 200, 58, true);

            closeGmod.Text = "When one game closes, close the other too";
            closeGmod.SetBounds(S(27), S(612), S(520), S(24));
            closeGmod.ForeColor = Dim;
            closeGmod.Checked = true;
            Controls.Add(closeGmod);
            status.SetBounds(S(27), S(640), S(560), S(36));
            status.ForeColor = Dim;
            Controls.Add(status);

            install.Click += delegate { Install(); };
            nvidia.Click += delegate { FixNvidia(); };
            stop.Click += delegate { StopGames(); };
            logs.Click += delegate { OpenLogs(); };
            uninstall.Click += delegate { Uninstall(); };
            // Things change behind the launcher's back (a game updated, a setting changed).
            Activated += delegate { Recheck(); };
            play.Click += delegate { Play(); };

            LoadSettings();
            if (!Finder.IsRdr2(rdr2Box.Text)) rdr2Box.Text = Finder.FindRdr2() ?? "";
            if (!Finder.IsGmod(gmodBox.Text)) gmodBox.Text = Finder.FindGmod() ?? "";
            rdr2Box.TextChanged += delegate { Recheck(); };
            gmodBox.TextChanged += delegate { Recheck(); };
            Recheck();

            timer.Interval = 1000;
            timer.Tick += delegate { Tick(); };
            timer.Start();
            FormClosing += delegate { SaveSettings(); };
        }

        // ---- layout helpers

        Label MakeLabel(string text, int x, int y, int w, int h, Font font, Color color)
        {
            Label l = new Label();
            l.Text = text;
            l.SetBounds(S(x), S(y), S(w), S(h));
            l.Font = font;
            l.ForeColor = color;
            l.BackColor = Color.Transparent;
            Controls.Add(l);
            return l;
        }

        void StyleButton(Button b, string text, int x, int y, int w, int h, bool primary)
        {
            b.Text = text;
            b.SetBounds(S(x), S(y), S(w), S(h));
            b.FlatStyle = FlatStyle.Flat;
            b.FlatAppearance.BorderColor = primary ? Red : Color.FromArgb(90, 78, 68);
            b.BackColor = primary ? Red : Panel;
            b.ForeColor = Ink;
            b.Font = primary ? new Font("Segoe UI", 15f, FontStyle.Bold) : Font;
            b.Cursor = Cursors.Hand;
            Controls.Add(b);
        }

        void PathRow(string name, TextBox box, int y, string hint, Func<string, bool> valid)
        {
            MakeLabel(name.ToUpperInvariant(), 27, y, 400, 18, new Font("Segoe UI", 8.5f, FontStyle.Bold), Dim);
            box.SetBounds(S(24), S(y + 20), S(560), S(26));
            box.BackColor = Panel;
            box.ForeColor = Ink;
            box.BorderStyle = BorderStyle.FixedSingle;
            Controls.Add(box);
            Button browse = new Button(), detect = new Button();
            StyleButton(browse, "Browse...", 592, y + 19, 98, 28, false);
            StyleButton(detect, "Detect", 698, y + 19, 98, 28, false);
            browse.Click += delegate
            {
                using (FolderBrowserDialog d = new FolderBrowserDialog())
                {
                    d.Description = hint;
                    if (Directory.Exists(box.Text)) d.SelectedPath = box.Text;
                    if (d.ShowDialog(this) != DialogResult.OK) return;
                    if (!valid(d.SelectedPath))
                    {
                        MessageBox.Show(this, "That is not it: " + hint.ToLowerInvariant() + " is wanted.", Product,
                            MessageBoxButtons.OK, MessageBoxIcon.Warning);
                        return;
                    }
                    box.Text = d.SelectedPath;
                }
            };
            detect.Click += delegate
            {
                string found = box == rdr2Box ? Finder.FindRdr2() : Finder.FindGmod();
                if (found == null)
                {
                    MessageBox.Show(this, name + " was not found automatically. Use Browse.", Product, MessageBoxButtons.OK,
                        MessageBoxIcon.Information);
                }
                else
                {
                    box.Text = found;
                }
            };
        }

        // ---- settings

        void LoadSettings()
        {
            try
            {
                foreach (string line in File.ReadAllLines(settingsFile))
                {
                    int eq = line.IndexOf('=');
                    if (eq < 0) continue;
                    string key = line.Substring(0, eq), value = line.Substring(eq + 1);
                    if (key == "rdr2") rdr2Box.Text = value;
                    if (key == "gmod") gmodBox.Text = value;
                    if (key == "close_gmod") closeGmod.Checked = value == "1";
                }
            }
            catch (Exception) { }
        }

        void SaveSettings()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(settingsFile));
                File.WriteAllLines(settingsFile, new string[] {
                    "rdr2=" + rdr2Box.Text, "gmod=" + gmodBox.Text, "close_gmod=" + (closeGmod.Checked ? "1" : "0") });
            }
            catch (Exception) { }
        }

        // ---- the checklist

        static bool SameFile(string a, string b)
        {
            try
            {
                if (!File.Exists(a) || !File.Exists(b)) return false;
                byte[] x = File.ReadAllBytes(a), y = File.ReadAllBytes(b);
                if (x.Length != y.Length) return false;
                for (int i = 0; i < x.Length; i++) if (x[i] != y[i]) return false;
                return true;
            }
            catch (Exception) { return false; }
        }

        static string Rdr2Settings()
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
                @"Rockstar Games\Red Dead Redemption 2\Settings\system.xml");
        }

        List<Check> RunChecks()
        {
            List<Check> list = new List<Check>();
            string rdr2 = rdr2Box.Text.Trim(), gmod = gmodBox.Text.Trim();
            bool haveRdr2 = Finder.IsRdr2(rdr2), haveGmod = Finder.IsGmod(gmod);
            string asi = Path.Combine(payload, @"RDR2\GarrysRedemption.asi");
            string module = Path.Combine(payload, @"GarrysMod\garrysmod\lua\bin\gmcl_gr_win64.dll");
            string addonInit = @"garrysmod\addons\garrys_redemption\lua\autorun\gr_init.lua";
            canInstall = File.Exists(asi) && File.Exists(module);
            needsInstall = false;

            list.Add(haveRdr2 ? new Check(Light.Good, "Red Dead Redemption 2 found", rdr2)
                              : new Check(Light.Bad, "Red Dead Redemption 2 not found", "Use Browse to point at the folder that holds RDR2.exe."));
            list.Add(haveGmod ? new Check(Light.Good, "Garry's Mod found", gmod)
                              : new Check(Light.Bad, "Garry's Mod not found", "Use Browse to point at the folder that holds the garrysmod folder."));

            if (haveGmod)
            {
                list.Add(Finder.GmodExe(gmod) != null
                    ? new Check(Light.Good, "Garry's Mod is the 64-bit version", "")
                    : new Check(Light.Bad, "Garry's Mod is not on the 64-bit branch",
                        "Steam: Garry's Mod, Properties, Betas, pick \"x86-64 - Chromium + 64-bit binaries\"."));
            }
            if (haveRdr2)
            {
                list.Add(File.Exists(Path.Combine(rdr2, "ScriptHookRDR2.dll"))
                    ? new Check(Light.Good, "Script Hook RDR2 is installed", "")
                    : new Check(Light.Bad, "Script Hook RDR2 is missing",
                        "Download it from its author (Alexander Blade, dev-c.com) and put ScriptHookRDR2.dll and dinput8.dll next to RDR2.exe. It cannot be shipped with the mod."));
                bool loader = File.Exists(Path.Combine(rdr2, "dinput8.dll")) || File.Exists(Path.Combine(rdr2, "version.dll")) ||
                              File.Exists(Path.Combine(rdr2, "winmm.dll"));
                if (!loader) list.Add(new Check(Light.Warn, "No ASI loader seen", "dinput8.dll from the Script Hook RDR2 download loads the mod's .asi file."));
            }

            if (!canInstall)
            {
                list.Add(new Check(Light.Warn, "No mod files next to this launcher",
                    "Keep the launcher in the unzipped release folder, beside its RDR2 and GarrysMod folders."));
            }
            if (haveRdr2 && canInstall)
            {
                bool same = SameFile(asi, Path.Combine(rdr2, "GarrysRedemption.asi"));
                needsInstall |= !same;
                list.Add(same ? new Check(Light.Good, "RDR2 plugin installed", "GarrysRedemption.asi")
                    : new Check(Light.Bad, File.Exists(Path.Combine(rdr2, "GarrysRedemption.asi")) ? "RDR2 plugin is a different version" : "RDR2 plugin not installed",
                        "Press \"Install / update mod files\"."));
            }
            if (haveGmod && canInstall)
            {
                bool same = SameFile(module, Path.Combine(gmod, @"garrysmod\lua\bin\gmcl_gr_win64.dll")) &&
                            SameFile(Path.Combine(payload, @"GarrysMod\" + addonInit), Path.Combine(gmod, addonInit));
                needsInstall |= !same;
                list.Add(same ? new Check(Light.Good, "Garry's Mod module and addon installed", "")
                    : new Check(Light.Bad, "Garry's Mod module or addon missing or a different version", "Press \"Install / update mod files\"."));
            }

            // RDR2's own display settings.
            bool vulkan = true;
            try
            {
                string xml = File.ReadAllText(Rdr2Settings());
                vulkan = xml.IndexOf("kSettingAPI_Vulkan", StringComparison.Ordinal) >= 0;
                Match m = Regex.Match(xml, "<windowed value=\"(\\d)\"");
                if (m.Success && m.Groups[1].Value == "0")
                {
                    list.Add(new Check(Light.Bad, "RDR2 is set to exclusive fullscreen",
                        "Garry's Mod's HUD cannot be drawn over it. RDR2: Settings, Graphics, Screen Type: Windowed Borderless."));
                }
                else if (m.Success)
                {
                    list.Add(new Check(Light.Good, "RDR2 screen type: " + (m.Groups[1].Value == "2" ? "windowed borderless" : "windowed"), ""));
                }
            }
            catch (Exception)
            {
                list.Add(new Check(Light.Info, "RDR2's settings were not found", "Start RDR2 once, and set Screen Type to Windowed Borderless."));
            }

            // The NVIDIA present method: only matters for Vulkan.
            nvidiaAdvice = null;
            if (!vulkan)
            {
                list.Add(new Check(Light.Good, "RDR2 runs on DirectX 12", "No driver setting is needed."));
            }
            else if (!Nvidia.Present())
            {
                list.Add(new Check(Light.Info, "No NVIDIA driver here",
                    "If Garry's Mod's HUD does not show over a full-screen borderless RDR2, switch RDR2's Graphics API to DirectX 12."));
            }
            else
            {
                uint value;
                string error = Nvidia.Access(null, out value);
                if (error != null)
                {
                    nvidiaAdvice = "manual";
                    list.Add(new Check(Light.Warn, "NVIDIA present method could not be read: " + error,
                        "By hand: NVIDIA Control Panel, Manage 3D settings, Program Settings, Red Dead Redemption 2, \"Vulkan/OpenGL present method\": \"Prefer layered on DXGI Swapchain\"."));
                }
                else if (value == Nvidia.LayeredOnDxgi)
                {
                    list.Add(new Check(Light.Good, "NVIDIA present method: layered on DXGI swap chain", "Windows can draw Garry's Mod's HUD over RDR2."));
                }
                else
                {
                    nvidiaAdvice = "fix";
                    list.Add(new Check(Light.Bad, "NVIDIA present method: " + (value == Nvidia.Auto ? "auto" : "native"),
                        "With RDR2 filling the monitor, the HUD and guns are not drawn over it. Press \"Fix the NVIDIA setting\" (it asks first), then restart RDR2."));
                }
            }
            return list;
        }

        void Recheck()
        {
            List<Check> list = RunChecks();
            checks.SuspendLayout();
            checks.Controls.Clear();
            int y = S(10);
            foreach (Check c in list)
            {
                Color color = c.Light == Light.Good ? Green : c.Light == Light.Warn ? Amber : c.Light == Light.Bad ? Red : Dim;
                Label dot = new Label();
                dot.Text = "\u25CF";
                dot.ForeColor = color;
                dot.SetBounds(S(12), y - 1, S(20), S(20));
                Label head = new Label();
                head.Text = c.Title;
                head.ForeColor = Ink;
                head.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold);
                head.SetBounds(S(34), y, S(700), S(20));
                checks.Controls.Add(dot);
                checks.Controls.Add(head);
                y += S(21);
                if (!string.IsNullOrEmpty(c.Detail))
                {
                    Label detail = new Label();
                    detail.Text = c.Detail;
                    detail.ForeColor = Dim;
                    detail.MaximumSize = new Size(S(700), 0);
                    detail.AutoSize = true;
                    detail.Location = new Point(S(34), y);
                    checks.Controls.Add(detail);
                    y += detail.PreferredHeight + S(2);
                }
                y += S(7);
            }
            checks.ResumeLayout();
            install.Enabled = canInstall && Finder.IsRdr2(rdr2Box.Text.Trim()) && Finder.IsGmod(gmodBox.Text.Trim());
            install.Text = needsInstall ? "Install / update mod files" : "Reinstall mod files";
            nvidia.Enabled = nvidiaAdvice != null;
        }

        // ---- actions

        static bool Running(string name)
        {
            return Process.GetProcessesByName(name).Length > 0;
        }

        static void CopyTree(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (string f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), true);
            foreach (string d in Directory.GetDirectories(from)) CopyTree(d, Path.Combine(to, Path.GetFileName(d)));
        }

        // Copies the mod's files into both games. Throws on the first thing that fails.
        public static void InstallFiles(string payload, string rdr2, string gmod)
        {
            // Replaced whole, so a Lua file removed from the mod does not linger.
            string addon = Path.Combine(gmod, @"garrysmod\addons\garrys_redemption");
            if (Directory.Exists(addon)) Directory.Delete(addon, true);
            CopyTree(Path.Combine(payload, "GarrysMod"), gmod);
            File.Copy(Path.Combine(payload, @"RDR2\GarrysRedemption.asi"), Path.Combine(rdr2, "GarrysRedemption.asi"), true);
        }

        void Install()
        {
            string rdr2 = rdr2Box.Text.Trim(), gmod = gmodBox.Text.Trim();
            if (Running("RDR2") || Running("gmod_win64") || Running("gmod"))
            {
                MessageBox.Show(this, "Close RDR2 and Garry's Mod first: their files are in use while they run.", Product,
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            try
            {
                InstallFiles(payload, rdr2, gmod);
            }
            catch (UnauthorizedAccessException)
            {
                // A game under Program Files: the copy is done again by this program started as
                // administrator, and Windows asks the user first.
                try
                {
                    ProcessStartInfo info = new ProcessStartInfo(Application.ExecutablePath,
                        "--install \"" + rdr2 + "\" \"" + gmod + "\"");
                    info.Verb = "runas";
                    info.UseShellExecute = true;
                    using (Process p = Process.Start(info))
                    {
                        p.WaitForExit();
                        if (p.ExitCode != 0) throw new IOException("the copy as administrator failed (code " + p.ExitCode + ")");
                    }
                }
                catch (Exception e)
                {
                    MessageBox.Show(this, "The files could not be installed: " + e.Message, Product, MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The files could not be installed: " + e.Message, Product, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            Recheck();
        }

        // Takes the mod's files out of both games. Script Hook and the games themselves stay.
        public static void RemoveFiles(string rdr2, string gmod)
        {
            string[] files = { Path.Combine(rdr2, "GarrysRedemption.asi"), Path.Combine(gmod, @"garrysmod\lua\bin\gmcl_gr_win64.dll"),
                Path.Combine(gmod, @"garrysmod\lua\bin\gmsv_gr_win64.dll"), Path.Combine(gmod, "play_gmod.bat") };
            foreach (string f in files) if (File.Exists(f)) File.Delete(f);
            string addon = Path.Combine(gmod, @"garrysmod\addons\garrys_redemption");
            if (Directory.Exists(addon)) Directory.Delete(addon, true);
        }

        void Uninstall()
        {
            string rdr2 = rdr2Box.Text.Trim(), gmod = gmodBox.Text.Trim();
            if (!Finder.IsRdr2(rdr2) || !Finder.IsGmod(gmod)) return;
            if (Running("RDR2") || Running("gmod_win64") || Running("gmod"))
            {
                MessageBox.Show(this, "Close RDR2 and Garry's Mod first.", Product, MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (MessageBox.Show(this, "Remove the mod's files from both games? Script Hook RDR2 and your saves are not touched.", Product,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            try
            {
                RemoveFiles(rdr2, gmod);
            }
            catch (UnauthorizedAccessException)
            {
                try
                {
                    ProcessStartInfo info = new ProcessStartInfo(Application.ExecutablePath, "--uninstall \"" + rdr2 + "\" \"" + gmod + "\"");
                    info.Verb = "runas";
                    info.UseShellExecute = true;
                    using (Process p = Process.Start(info)) p.WaitForExit();
                }
                catch (Exception e)
                {
                    MessageBox.Show(this, "The files could not be removed: " + e.Message, Product, MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "The files could not be removed: " + e.Message, Product, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            Recheck();
        }

        void FixNvidia()
        {
            const string manual = "NVIDIA Control Panel, Manage 3D settings, Program Settings, pick Red Dead Redemption 2 " +
                "(add RDR2.exe if it is not listed), set \"Vulkan/OpenGL present method\" to \"Prefer layered on DXGI Swapchain\", Apply, then restart RDR2.";
            if (nvidiaAdvice == "manual")
            {
                MessageBox.Show(this, "This launcher could not reach the driver's settings. By hand:\n\n" + manual, Product,
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            DialogResult answer = MessageBox.Show(this,
                "Change one NVIDIA driver setting for Red Dead Redemption 2 only?\n\n" +
                "    Vulkan/OpenGL present method: Prefer layered on DXGI Swapchain\n\n" +
                "It lets Windows draw Garry's Mod's HUD and guns over an RDR2 that fills the whole monitor. Nothing else in " +
                "the driver is touched, other games are not affected, and you can set it back in the NVIDIA Control Panel " +
                "(Manage 3D settings, Program Settings, Red Dead Redemption 2).\n\nRDR2 has to be restarted afterwards.",
                Product, MessageBoxButtons.YesNo, MessageBoxIcon.Question);
            if (answer != DialogResult.Yes) return;
            uint value;
            string error = Nvidia.Access(Nvidia.LayeredOnDxgi, out value);
            if (error != null)
            {
                MessageBox.Show(this, "The driver did not take the setting (" + error + "). By hand:\n\n" + manual, Product,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
            }
            else if (Running("RDR2"))
            {
                MessageBox.Show(this, "Done. Restart RDR2 for it to take effect.", Product, MessageBoxButtons.OK, MessageBoxIcon.Information);
            }
            Recheck();
        }

        void StopGames()
        {
            if (!Running("RDR2") && !Running("gmod_win64") && !Running("gmod")) return;
            if (MessageBox.Show(this, "Close Garry's Mod and RDR2 now? Unsaved RDR2 progress is lost.", Product,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            foreach (string name in new string[] { "gmod_win64", "gmod", "RDR2" })
            {
                foreach (Process p in Process.GetProcessesByName(name))
                {
                    try { p.Kill(); } catch (Exception) { }
                }
            }
            waitingToStartGmod = false;
        }

        void OpenLogs()
        {
            string plugin = Path.Combine(Path.GetTempPath(), "GarrysRedemption.log");
            string module = Path.Combine(gmodBox.Text.Trim(), @"garrysmod\lua\bin\GarrysRedemption_gmod.log");
            bool any = false;
            foreach (string log in new string[] { plugin, module })
            {
                if (!File.Exists(log)) continue;
                any = true;
                try { Process.Start("notepad.exe", "\"" + log + "\""); } catch (Exception) { }
            }
            if (!any) MessageBox.Show(this, "No logs yet: they are written when the games run with the mod.", Product);
        }

        void StartGmod()
        {
            string gmod = gmodBox.Text.Trim();
            string exe = Finder.GmodExe(gmod);
            if (exe == null) return;
            ProcessStartInfo info = new ProcessStartInfo(exe, GmodArgs);
            info.WorkingDirectory = gmod;
            info.UseShellExecute = false;
            Process.Start(info);
            startedGmod = true;
            gmodTucked = false;
        }

        void Play()
        {
            Recheck();
            string rdr2 = rdr2Box.Text.Trim(), gmod = gmodBox.Text.Trim();
            if (!Finder.IsRdr2(rdr2) || !Finder.IsGmod(gmod) || Finder.GmodExe(gmod) == null)
            {
                MessageBox.Show(this, "Both games have to be found first (see the checklist).", Product, MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (needsInstall)
            {
                if (MessageBox.Show(this, "The mod's files are not installed (or are another version). Install them now?", Product,
                        MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                Install();
                if (needsInstall) return;
            }
            if (!File.Exists(Path.Combine(rdr2, "ScriptHookRDR2.dll")))
            {
                MessageBox.Show(this, "Script Hook RDR2 is missing: without it RDR2 does not load the mod. See the checklist.", Product,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            try
            {
                if (!Running("RDR2"))
                {
                    ProcessStartInfo info = new ProcessStartInfo(Path.Combine(rdr2, "RDR2.exe"));
                    info.WorkingDirectory = rdr2;
                    info.UseShellExecute = true;
                    Process.Start(info);
                }
                // Garry's Mod waits until RDR2's window is there: started at once it would sit in
                // front of RDR2's loading screens.
                waitingToStartGmod = !Running("gmod_win64") && !Running("gmod");
                sawBoth = false;
                status.Text = "Starting RDR2. Load STORY mode; Garry's Mod starts by itself and hides once the two are linked.";
            }
            catch (Exception e)
            {
                MessageBox.Show(this, "RDR2 could not be started: " + e.Message, Product, MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        string LinkLine()
        {
            try
            {
                string log = Path.Combine(gmodBox.Text.Trim(), @"garrysmod\lua\bin\GarrysRedemption_gmod.log");
                using (FileStream f = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
                {
                    long take = Math.Min(f.Length, 6000);
                    f.Seek(-take, SeekOrigin.End);
                    byte[] bytes = new byte[take];
                    f.Read(bytes, 0, (int)take);
                    string[] lines = Encoding.UTF8.GetString(bytes).Split('\n');
                    for (int i = lines.Length - 1; i >= 0; i--)
                    {
                        if (lines[i].Contains("link: ") && lines[i].Contains(" -> ")) return lines[i].Contains("-> connected") ? "linked" : "not linked";
                    }
                }
            }
            catch (Exception) { }
            return "not linked";
        }

        void Tick()
        {
            bool rdr2 = Running("RDR2"), gmod = Running("gmod_win64") || Running("gmod");
            IntPtr rdr2Window = FindWindow("sgaWindow", null);
            if (rdr2 && gmod) sawBoth = true;

            if (waitingToStartGmod && rdr2Window != IntPtr.Zero)
            {
                waitingToStartGmod = false;
                StartGmod();
            }
            // Out of RDR2's way until the module hides it for good: behind every other window, and
            // RDR2 back in front. Not minimised: a minimised Garry's Mod draws no frames, and
            // then there is no HUD to show over RDR2 (seen: 0 presents).
            if (startedGmod && !gmodTucked)
            {
                IntPtr gmodWindow = FindWindow("Valve001", null);
                if (gmodWindow != IntPtr.Zero && IsWindowVisible(gmodWindow))
                {
                    SetWindowPos(gmodWindow, new IntPtr(1), 0, 0, 0, 0, 0x0001 | 0x0002 | 0x0010);  // HWND_BOTTOM; no size, move, activate
                    if (rdr2Window != IntPtr.Zero) SetForegroundWindow(rdr2Window);
                    gmodTucked = true;
                }
            }
            // The two belong together: once both have run, one closing closes the other. RDR2 does
            // not answer a polite request to close, so it is ended; the pair was the user's to end.
            if (sawBoth && rdr2 != gmod)
            {
                sawBoth = false;
                startedGmod = false;
                if (closeGmod.Checked)
                {
                    foreach (string name in new string[] { "gmod_win64", "gmod", "RDR2" })
                    {
                        foreach (Process p in Process.GetProcessesByName(name))
                        {
                            try { p.Kill(); } catch (Exception) { }
                        }
                    }
                }
            }

            stop.Enabled = rdr2 || gmod;
            play.Text = rdr2 && gmod ? "RUNNING" : "PLAY";
            play.Enabled = !(rdr2 && gmod);
            if (rdr2 || gmod || waitingToStartGmod)
            {
                status.Text = "RDR2: " + (rdr2 ? "running" : "not running") + "     Garry's Mod: " +
                    (gmod ? "running" : waitingToStartGmod ? "starts when RDR2's window appears" : "not running") +
                    (rdr2 && gmod ? "     Bridge: " + LinkLine() + (LinkLine() == "linked" ? "" : " (load story mode)") : "");
            }
            else if (!status.Text.StartsWith("Starting"))
            {
                status.Text = "Ready. PLAY starts RDR2, then Garry's Mod.";
            }
        }
    }

    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDPIAware();

        [STAThread]
        static int Main(string[] args)
        {
            // The elevated copy (MainForm.Install): no window, only the files.
            if (args.Length == 3 && args[0] == "--install")
            {
                try
                {
                    MainForm.InstallFiles(Path.GetDirectoryName(Application.ExecutablePath), args[1], args[2]);
                    return 0;
                }
                catch (Exception)
                {
                    return 1;
                }
            }
            if (args.Length == 3 && args[0] == "--uninstall")
            {
                try
                {
                    MainForm.RemoveFiles(args[1], args[2]);
                    return 0;
                }
                catch (Exception)
                {
                    return 1;
                }
            }
            // Read-only self-test for the build: what the checks find, as text.
            if (args.Length == 1 && args[0] == "--report")
            {
                uint value;
                string error = Nvidia.Present() ? Nvidia.Access(null, out value) : "no nvapi64.dll";
                if (error == null) Nvidia.Access(null, out value); else value = 99;
                File.WriteAllText(Path.Combine(Path.GetTempPath(), "GarrysRedemptionLauncher.txt"),
                    "rdr2=" + Finder.FindRdr2() + "\r\ngmod=" + Finder.FindGmod() + "\r\nnvidia_error=" + error +
                    "\r\nnvidia_present_method=" + value + "\r\n");
                return 0;
            }
            SetProcessDPIAware();
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
            return 0;
        }
    }
}
