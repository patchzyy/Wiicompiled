namespace WiiCompiled.Setup.Windows;
// Adapter for the shared Windows/Mac compile-input fingerprint implementation.
internal static class InputValidation
{
    public static string Sha256File(string path) => MacSetup.Hash(path).ToLowerInvariant();
}
