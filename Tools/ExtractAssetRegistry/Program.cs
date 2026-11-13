// ExtractAssetRegistry: writes the game's premade AssetRegistry.bin (root one, not a plugin's) out of its containers.
// Usage: ExtractAssetRegistry <Paks dir> <aes key file> <out file> [--game GAME_Game]
// The key file is read and never printed. Needs only CUE4Parse (the file lives in the first-party container pakchunk0).
using CUE4Parse.Encryption.Aes;
using CUE4Parse.FileProvider;
using CUE4Parse.UE4.Versions;

if (args.Length < 3) { Console.WriteLine("Usage: ExtractAssetRegistry <Paks dir> <aes key file> <out file> [--game GAME_Game]"); return 2; }
string paks = args[0], keyFile = args[1], outFile = args[2], game = "GAME_Game";
for (int i = 3; i + 1 < args.Length; i++) if (args[i] == "--game") game = args[++i];
var key = new FAesKey(File.ReadAllText(keyFile).Trim());
var provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, new VersionContainer(Enum.Parse<EGame>(game)), StringComparer.OrdinalIgnoreCase);
provider.Initialize();
int mounted = 0;
foreach (var g in provider.RequiredKeys) mounted += provider.SubmitKey(g, key);
Console.WriteLine($"mounted {mounted} groups, {provider.Files.Count} files");
var ar = provider.Files.Keys.FirstOrDefault(k => k.EndsWith("AssetRegistry.bin", StringComparison.OrdinalIgnoreCase) && !k.Contains("/Plugins/", StringComparison.OrdinalIgnoreCase));
if (ar == null) { Console.WriteLine("AssetRegistry.bin not found (wrong key or Paks dir?)"); return 1; }
File.WriteAllBytes(outFile, provider.SaveAsset(ar));
Console.WriteLine($"wrote {outFile} from {ar}");
return 0;
