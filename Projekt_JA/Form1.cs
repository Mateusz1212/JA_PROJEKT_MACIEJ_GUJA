using System;
using System.IO;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows.Forms;
using System.Drawing;

namespace Projekt_JA
{
    public partial class Form1 : Form
    {
        private bool isCompression = true;
        private string sourcePath = string.Empty;
        private string destinationPath = string.Empty;
        private string Pathtozip = string.Empty;

        // Rozmiary groupBox3
        private const int GroupBox3FullHeight = 95;
        private const int GroupBox3SmallHeight = 61;

        // ============================================================
        // Kontrolki dodawane w kodzie (nie w Designerze):
        //   progressBar – pasek postepu (0-100)
        //   logTextBox  – pole tekstowe z logiem operacji
        // ============================================================
        private ProgressBar progressBar = null!;
        private TextBox logTextBox = null!;

        // ============================================================
        // P/Invoke – wywolania do Logic.dll
        //
        // UWAGA: Nazwa DLL musi zgadzac sie z nazwa projektu w VS
        //        (domyslnie "Logic.dll"). Zmien jesli projekt nazywa sie inaczej.
        //
        // StartCompression:
        //   sourceFolder  – folder z obrazkami
        //   outputFolder  – folder tymczasowy na pliki .lz77
        //   useASM        – true = Dll_ASM.dll, false = Dll_CPP.dll
        //   numThreads    – liczba workerow z suwaka GUI
        //   progressCb    – callback (percent: int)
        //   logCb         – callback (message: wstring)
        //
        // StartDecompression:
        //   sourceFolder  – folder tymczasowy z wypakowanymi plikami .lz77
        //   outputFolder  – folder wynikowy dla zdekompresowanych .bmp
        //   pozostale parametry jak wyzej
        // ============================================================
        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate void ProgressCallback(int percent);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        private delegate void LogCallback(
            [MarshalAs(UnmanagedType.LPWStr)] string message);

        [DllImport("CppLogicDll.dll",
                   CallingConvention = CallingConvention.StdCall,
                   CharSet = CharSet.Unicode)]
        private static extern void StartCompression(
            string sourceFolder,
            string outputFolder,
            bool useASM,
            int numThreads,
            ProgressCallback progressCb,
            LogCallback logCb,
            out long outElapsedMs);

        [DllImport("CppLogicDll.dll",
                   CallingConvention = CallingConvention.StdCall,
                   CharSet = CharSet.Unicode)]
        private static extern void StartDecompression(
            string sourceFolder,
            string outputFolder,
            bool useASM,
            int numThreads,
            ProgressCallback progressCb,
            LogCallback logCb,
            out long outElapsedMs);

        // ============================================================
        public Form1()
        {
            InitializeComponent();
        }

        // ============================================================
        // Form1_Load – inicjalizacja suwaka, ikon oraz dodatkowych
        //              kontrolek (progressBar, logTextBox)
        // ============================================================
        private void Form1_Load(object sender, EventArgs e)
        {
            // Wczytaj ikone z pliku icon.png w folderze aplikacji
            string iconPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "icon.png");
            if (File.Exists(iconPath))
            {
                using Bitmap bmp = new Bitmap(iconPath);
                this.Icon = ConvertBitmapToIcon(bmp);
            }

            // Suwak liczby watkow (1–64, domyslnie = liczba procesorow)
            int processorCount = Environment.ProcessorCount;
            trackBar1.Minimum = 1;
            trackBar1.Maximum = 64;
            trackBar1.Value = Math.Min(processorCount, 64);
            trackBar1.TickFrequency = 1;
            label6.Text = $"Wybrano: {trackBar1.Value}";

            trackBar1.ValueChanged += trackBar1_ValueChanged;
            button2.Click += button2_Click;
            button3.Click += button3_Click;

            // --- Dodaj pasek postepu ponizej przycisku Kompresuj/Dekompresuj ---
            progressBar = new ProgressBar
            {
                Location = new Point(12, button1.Bottom + 10),
                Size = new Size(ClientSize.Width - 24, 23),
                Minimum = 0,
                Maximum = 100,
                Value = 0
            };
            Controls.Add(progressBar);

            // --- Dodaj wieloliniowe pole z logiem ---
            logTextBox = new TextBox
            {
                Location = new Point(12, progressBar.Bottom + 6),
                Size = new Size(ClientSize.Width - 24, 110),
                Multiline = true,
                ScrollBars = ScrollBars.Vertical,
                ReadOnly = true,
                BackColor = SystemColors.Window,
                Font = new Font("Consolas", 8.5f)
            };
            Controls.Add(logTextBox);

            // Dostosuj wysokosc okna do nowych kontrolek
            ClientSize = new Size(ClientSize.Width, logTextBox.Bottom + 12);
            MinimumSize = new Size(530, ClientSize.Height + 40);
        }

        // ============================================================
        // Konwersja Bitmap → Icon (PNG embedded, Vista+)
        // ============================================================
        private static Icon ConvertBitmapToIcon(Bitmap bmp)
        {
            using MemoryStream ms = new MemoryStream();
            ms.Write(new byte[] { 0, 0 }, 0, 2);       // Reserved
            ms.Write(new byte[] { 1, 0 }, 0, 2);       // Type: ICO
            ms.Write(new byte[] { 1, 0 }, 0, 2);       // Count: 1 obraz

            int width = bmp.Width >= 256 ? 0 : bmp.Width;
            int height = bmp.Height >= 256 ? 0 : bmp.Height;

            using MemoryStream pngStream = new MemoryStream();
            bmp.Save(pngStream, System.Drawing.Imaging.ImageFormat.Png);
            byte[] pngBytes = pngStream.ToArray();
            int imageOffset = 6 + 16;

            ms.WriteByte((byte)width);
            ms.WriteByte((byte)height);
            ms.WriteByte(0);
            ms.WriteByte(0);
            ms.Write(new byte[] { 1, 0 }, 0, 2);
            ms.Write(new byte[] { 32, 0 }, 0, 2);
            ms.Write(BitConverter.GetBytes(pngBytes.Length), 0, 4);
            ms.Write(BitConverter.GetBytes(imageOffset), 0, 4);
            ms.Write(pngBytes, 0, pngBytes.Length);
            ms.Seek(0, SeekOrigin.Begin);
            return new Icon(ms);
        }

        // ============================================================
        // Suwak liczby watkow
        // ============================================================
        private void trackBar1_ValueChanged(object sender, EventArgs e)
        {
            label6.Text = $"Wybrano: {trackBar1.Value}";
        }

        // ============================================================
        // Przelaczanie trybu: Kompresja / Dekompresja
        // ============================================================
        private void radioButton3_CheckedChanged(object sender, EventArgs e)
        {
            isCompression = true;
            label2.Text = "Wybor folderu z obrazkami";
            label3.Visible = true;
            button3.Visible = true;
            button1.Text = "Kompresuj";
            AdjustLayout(true);
        }

        private void radioButton4_CheckedChanged(object sender, EventArgs e)
        {
            isCompression = false;
            label2.Text = "Wybor pliku ZIP do dekompresji";
            label3.Visible = false;
            button3.Visible = false;
            button1.Text = "Dekompresuj";
            AdjustLayout(false);
        }

        // ============================================================
        // AdjustLayout – zmiana rozmiaru groupBox3 i przesuniecie
        //                kontrolek lezacych ponizej (w tym progressBar
        //                i logTextBox dodanych w kodzie)
        // ============================================================
        private void AdjustLayout(bool fullSize)
        {
            int newHeight = fullSize ? GroupBox3FullHeight : GroupBox3SmallHeight;
            int delta = newHeight - groupBox3.Height;
            if (delta == 0) return;

            groupBox3.Height = newHeight;

            // Kontrolki z Designera lezace ponizej groupBox3
            Control[] staticControls = { groupBox1, label1, trackBar1,
                                         label4, label5, label6, button1, labelElapsedTime };
            foreach (var ctrl in staticControls)
                ctrl.Top += delta;

            // Kontrolki dodane dynamicznie w Form1_Load
            if (progressBar != null) progressBar.Top += delta;
            if (logTextBox != null) logTextBox.Top += delta;

            ClientSize = new Size(ClientSize.Width, ClientSize.Height + delta);
        }

        // ============================================================
        // button2 – wybor zrodla (folder z obrazkami lub plik ZIP)
        // ============================================================
        private void button2_Click(object sender, EventArgs e)
        {
            if (isCompression)
            {
                using FolderBrowserDialog folderDialog = new FolderBrowserDialog();
                folderDialog.Description = "Wybierz folder ze zdjeciami";
                if (folderDialog.ShowDialog() == DialogResult.OK)
                {
                    sourcePath = folderDialog.SelectedPath;
                    label2.Text = sourcePath;
                }
            }
            else
            {
                using OpenFileDialog openDialog = new OpenFileDialog();
                openDialog.Title = "Wybierz plik ZIP";
                openDialog.Filter = "Pliki ZIP|*.zip";
                if (openDialog.ShowDialog() == DialogResult.OK)
                {
                    Pathtozip = openDialog.FileName;
                    label2.Text = Pathtozip;
                }
            }
        }

        // ============================================================
        // button3 – wybor miejsca zapisu archiwum ZIP (tylko kompresja)
        // ============================================================
        private void button3_Click(object sender, EventArgs e)
        {
            using SaveFileDialog saveDialog = new SaveFileDialog();
            saveDialog.Title = "Wybierz lokalizacje zapisu pliku ZIP";
            saveDialog.Filter = "Pliki ZIP|*.zip";
            saveDialog.DefaultExt = "zip";
            if (saveDialog.ShowDialog() == DialogResult.OK)
            {
                destinationPath = saveDialog.FileName;
                label3.Text = destinationPath;
            }
        }

        // ============================================================
        // button1_Click – glowna akcja: Kompresuj lub Dekompresuj
        //
        // Przepływ kompresji:
        //   1. Walidacja sciezek.
        //   2. Utworzenie folderu tymczasowego.
        //   3. Wywolanie StartCompression z Logic.dll w Task.Run:
        //      – logic.cpp tworzy numThreads workerow,
        //      – kazdy worker kompresuje jeden obrazek przez LZ77
        //        (ładuje DLL Dll_CPP.dll lub Dll_ASM.dll),
        //      – wynik zapisuje jako plik .lz77 w folderze tymczasowym.
        //   4. C# pakuje pliki .lz77 do archiwum ZIP (System.IO.Compression).
        //   5. Usniecie folderu tymczasowego.
        //
        // Przepływ dekompresji:
        //   1. Walidacja sciezki ZIP.
        //   2. Wypakowanie archiwum ZIP do folderu tymczasowego (C#).
        //   3. Utworzenie folderu wynikowego (ta sama nazwa co ZIP, bez ext).
        //   4. Wywolanie StartDecompression z Logic.dll w Task.Run:
        //      – logic.cpp tworzy numThreads workerow,
        //      – kazdy worker dekompresuje jeden plik .lz77 przez LZ77
        //        i zapisuje go jako .bmp w folderze wynikowym.
        //   5. Usuniecie folderu tymczasowego.
        // ============================================================
        private async void button1_Click(object sender, EventArgs e)
        {
            // Walidacja sciezek przed startem
            if (isCompression && string.IsNullOrWhiteSpace(sourcePath))
            {
                MessageBox.Show("Wybierz sciezke zrodlowa.",
                                "Blad", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (isCompression && string.IsNullOrWhiteSpace(destinationPath))
            {
                MessageBox.Show("Wybierz sciezke docelowa (plik .zip).",
                                "Blad", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (!isCompression && string.IsNullOrWhiteSpace(Pathtozip))
            {
                MessageBox.Show("Wybierz sciezke pliku .zip!",
                                "Blad", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            // Odczytaj parametry z GUI przed przelaczeniem na inny watek
            bool useASM = radioButton2.Checked;
            int numThreads = trackBar1.Value;
            string src = sourcePath;
            string dst = destinationPath;
            string zip = Pathtozip;

            // Zablokuj przycisk na czas operacji
            button1.Enabled = false;
            labelElapsedTime.Visible = false;
            logTextBox.Clear();
            progressBar.Value = 0;

            if (isCompression)
                await Task.Run(() => RunCompression(src, dst, useASM, numThreads));
            else
                await Task.Run(() => RunDecompression(zip, useASM, numThreads));

            button1.Enabled = true;
        }

        // ============================================================
        // RunCompression (wykonywana w tle przez Task.Run)
        //
        // 1. Tworzy folder tymczasowy.
        // 2. Wola StartCompression z Logic.dll — ta tworzy N workerow
        //    i kompresuje obrazki do folderu tymczasowego (.lz77).
        // 3. Pakuje pliki .lz77 do archiwum ZIP w sciezce docelowej.
        // 4. Usuwa folder tymczasowy.
        // ============================================================
        private void RunCompression(string sourceFolder, string zipPath,
                                    bool useASM, int numThreads)
        {
            string tempFolder = Path.Combine(
                Path.GetTempPath(),
                "lz77_comp_" + Guid.NewGuid().ToString("N"));

            Directory.CreateDirectory(tempFolder);

            try
            {
                AppendLog("=== Kompresja: start ===");
                AppendLog($"Zrodlo:   {sourceFolder}");
                AppendLog($"Wyjscie:  {zipPath}");
                AppendLog($"Watkow:   {numThreads}  |  DLL: {(useASM ? "ASM" : "C++")}");
                AppendLog("");

                // Callbacki – delegaty musza zyc przez caly czas wywolania
                // StartCompression (synchronicznego), wiec lokalne zmienne
                // na stosie zarzadzanym sa bezpieczne (GC ich nie zbierze).
                ProgressCallback progressCb = percent => SetProgress(percent);
                LogCallback logCb = message => AppendLog(message);

                // Wywolaj logic.cpp – tworzy N workerow, kompresuje obrazki
                StartCompression(sourceFolder, tempFolder,
                                 useASM, numThreads,
                                 progressCb, logCb,
                                 out long elapsedMs);
                SetElapsedTime(elapsedMs);

                // --- Pakuj wyniki do archiwum ZIP (System.IO.Compression) ---
                AppendLog("");
                AppendLog("Pakowanie plikow .lz77 do archiwum ZIP...");

                if (File.Exists(zipPath))
                    File.Delete(zipPath);

                string[] lz77Files = Directory.GetFiles(tempFolder, "*.lz77");

                if (lz77Files.Length == 0)
                {
                    AppendLog("BLAD: Brak plikow .lz77 po kompresji – sprawdz logi powyzej.");
                    return;
                }

                using (ZipArchive archive = ZipFile.Open(zipPath, ZipArchiveMode.Create))
                {
                    foreach (string file in lz77Files)
                    {
                        // Dodaj plik .lz77 jako wpis w archiwum ZIP
                        archive.CreateEntryFromFile(file, Path.GetFileName(file));
                    }
                }

                AppendLog($"Gotowe! Archiwum ZIP: {zipPath}");
                AppendLog($"Liczba skompresowanych obrazkow: {lz77Files.Length}");
            }
            catch (Exception ex)
            {
                AppendLog($"WYJATEK: {ex.Message}");
            }
            finally
            {
                // Usun folder tymczasowy niezaleznie od wyniku
                try { Directory.Delete(tempFolder, recursive: true); }
                catch { /* ignoruj bledy czyszczenia */ }
            }
        }

        // ============================================================
        // RunDecompression (wykonywana w tle przez Task.Run)
        //
        // 1. Wypakuje archiwum ZIP do folderu tymczasowego (C#).
        // 2. Tworzy folder wynikowy o tej samej nazwie co ZIP
        //    (bez rozszerzenia), w tej samej lokalizacji co plik ZIP.
        // 3. Wola StartDecompression z Logic.dll — ta tworzy N workerow
        //    i dekompresuje pliki .lz77 do folderu wynikowego (.bmp).
        // 4. Usuwa folder tymczasowy.
        // ============================================================
        private void RunDecompression(string zipPath, bool useASM, int numThreads)
        {
            // Folder wynikowy: ta sama lokalizacja i nazwa co ZIP, bez ".zip"
            string zipDir = Path.GetDirectoryName(zipPath)!;
            string zipNameNoExt = Path.GetFileNameWithoutExtension(zipPath);
            string outputFolder = Path.Combine(zipDir, zipNameNoExt);

            string tempFolder = Path.Combine(
                Path.GetTempPath(),
                "lz77_decomp_" + Guid.NewGuid().ToString("N"));

            Directory.CreateDirectory(tempFolder);

            try
            {
                AppendLog("=== Dekompresja: start ===");
                AppendLog($"Zrodlo:   {zipPath}");
                AppendLog($"Wyjscie:  {outputFolder}");
                AppendLog($"Watkow:   {numThreads}  |  DLL: {(useASM ? "ASM" : "C++")}");
                AppendLog("");

                // --- Wypakowywanie ZIP do folderu tymczasowego (C#) ---
                AppendLog("Wypakowywanie archiwum ZIP...");
                using (ZipArchive archive = ZipFile.OpenRead(zipPath))
                {
                    foreach (ZipArchiveEntry entry in archive.Entries)
                    {
                        // Wypakowuj tylko pliki .lz77 (ignoruj inne wpisy)
                        if (!entry.Name.EndsWith(".lz77",
                                StringComparison.OrdinalIgnoreCase))
                            continue;

                        string destFile = Path.Combine(tempFolder, entry.Name);
                        entry.ExtractToFile(destFile, overwrite: true);
                    }
                }

                // Sprawdz, czy wypakowano jakies pliki .lz77
                string[] lz77Files = Directory.GetFiles(tempFolder, "*.lz77");
                if (lz77Files.Length == 0)
                {
                    AppendLog("BLAD: Archiwum ZIP nie zawiera plikow .lz77.");
                    return;
                }
                AppendLog($"Wypakowano {lz77Files.Length} plikow .lz77.");

                // Upewnij sie, ze folder wynikowy istnieje
                Directory.CreateDirectory(outputFolder);

                // Callbacki – jak w RunCompression, lokalne zmienne sa bezpieczne
                ProgressCallback progressCb = percent => SetProgress(percent);
                LogCallback logCb = message => AppendLog(message);

                // Wywolaj logic.cpp – tworzy N workerow, dekompresuje .lz77 → .bmp
                StartDecompression(tempFolder, outputFolder,
                                   useASM, numThreads,
                                   progressCb, logCb,
                                   out long elapsedMs);
                SetElapsedTime(elapsedMs);

                AppendLog("");
                AppendLog($"Gotowe! Zdekompresowane obrazy w: {outputFolder}");
            }
            catch (Exception ex)
            {
                AppendLog($"WYJATEK: {ex.Message}");
            }
            finally
            {
                try { Directory.Delete(tempFolder, recursive: true); }
                catch { /* ignoruj bledy czyszczenia */ }
            }
        }

        // ============================================================
        // Pomocnicze: thread-safe aktualizacja paska postepu i log
        // ============================================================
        private void SetProgress(int percent)
        {
            int clamped = Math.Max(0, Math.Min(100, percent));
            if (progressBar.InvokeRequired)
                progressBar.Invoke(new Action(() => progressBar.Value = clamped));
            else
                progressBar.Value = clamped;
        }

        private void AppendLog(string message)
        {
            if (logTextBox.InvokeRequired)
                logTextBox.Invoke(new Action(() =>
                    logTextBox.AppendText(message + Environment.NewLine)));
            else
                logTextBox.AppendText(message + Environment.NewLine);
        }

        // Wyswietla czas zwrocony przez logic.cpp (tylko wywolania compFn/decompFn)
        private void SetElapsedTime(long milliseconds)
        {
            string text = $"Czas wykonania: {milliseconds} ms";
            if (labelElapsedTime.InvokeRequired)
                labelElapsedTime.Invoke(new Action(() =>
                {
                    labelElapsedTime.Text = text;
                    labelElapsedTime.Visible = true;
                }));
            else
            {
                labelElapsedTime.Text = text;
                labelElapsedTime.Visible = true;
            }
        }

        // ============================================================
        // Handlery stubowe (podpiete w Designerze, nieuzywane)
        // ============================================================
        private void radioButton1_CheckedChanged(object sender, EventArgs e) { }
        private void radioButton2_CheckedChanged(object sender, EventArgs e) { }
        private void label1_Click(object sender, EventArgs e) { }
        private void label3_Click(object sender, EventArgs e) { }
        private void label2_Click(object sender, EventArgs e) { }
        private void label6_Click(object sender, EventArgs e) { }
        private void label5_Click(object sender, EventArgs e) { }

        private void groupBox2_Enter(object sender, EventArgs e)
        {

        }
    }
}