namespace EchoBridge;

internal sealed record AudioDeviceItem(string Id, string Name)
{
    public override string ToString() => string.IsNullOrWhiteSpace(Name) ? Id : Name;
}
