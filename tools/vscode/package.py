"""Packages the VS Code extension as a .vsix, without needing vsce or npm.

    py -I tools/vscode/package.py             # writes build/vscode/calx-<version>.vsix
    py -I tools/vscode/package.py --install   # and installs it with `code --install-extension`

A .vsix is a zip holding the extension under `extension/`, a manifest, and a content-types file.
The icons come from assets/logo, so they aren't copied into this folder.
"""

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import zipfile
from xml.sax.saxutils import escape

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
LOGO = ROOT / "assets" / "logo"

# What goes in the package, as (path in the package, file on disk).
FILES = [
    ("package.json", HERE / "package.json"),
    ("extension.js", HERE / "extension.js"),
    ("compiler.js", HERE / "compiler.js"),
    ("language-configuration.json", HERE / "language-configuration.json"),
    ("syntaxes/calx.tmLanguage.json", HERE / "syntaxes" / "calx.tmLanguage.json"),
    ("syntaxes/calx.markdown.tmLanguage.json", HERE / "syntaxes" / "calx.markdown.tmLanguage.json"),
    ("README.md", HERE / "README.md"),
    ("LICENSE.txt", ROOT / "LICENSE"),
    ("images/icon.png", LOGO / "calx-avatar-512.png"),
    ("images/calx-mark.svg", LOGO / "calx-mark.svg"),
    ("images/calx-mark-white.svg", LOGO / "calx-mark-white.svg"),
]

CONTENT_TYPES = """<?xml version="1.0" encoding="utf-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension=".json" ContentType="application/json"/>
<Default Extension=".js" ContentType="application/javascript"/>
<Default Extension=".md" ContentType="text/markdown"/>
<Default Extension=".txt" ContentType="text/plain"/>
<Default Extension=".png" ContentType="image/png"/>
<Default Extension=".svg" ContentType="image/svg+xml"/>
<Default Extension=".vsixmanifest" ContentType="text/xml"/>
</Types>
"""


def manifest(pkg):
    return f"""<?xml version="1.0" encoding="utf-8"?>
<PackageManifest Version="2.0.0" xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">
  <Metadata>
    <Identity Language="en-US" Id="{escape(pkg["name"])}" Version="{escape(pkg["version"])}" Publisher="{escape(pkg["publisher"])}"/>
    <DisplayName>{escape(pkg["displayName"])}</DisplayName>
    <Description xml:space="preserve">{escape(pkg["description"])}</Description>
    <Tags>calx</Tags>
    <Categories>{escape(",".join(pkg["categories"]))}</Categories>
    <GalleryFlags>Public</GalleryFlags>
    <Properties>
      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="{escape(pkg["engines"]["vscode"])}"/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value=""/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value=""/>
      <Property Id="Microsoft.VisualStudio.Code.ExtensionKind" Value="workspace"/>
      <Property Id="Microsoft.VisualStudio.Code.LocalizedLanguages" Value=""/>
      <Property Id="Microsoft.VisualStudio.Services.Links.Source" Value="{escape(pkg["repository"]["url"])}"/>
    </Properties>
    <License>extension/LICENSE.txt</License>
    <Icon>extension/images/icon.png</Icon>
  </Metadata>
  <Installation>
    <InstallationTarget Id="Microsoft.VisualStudio.Code"/>
  </Installation>
  <Dependencies/>
  <Assets>
    <Asset Type="Microsoft.VisualStudio.Code.Manifest" Path="extension/package.json" Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Content.Details" Path="extension/README.md" Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Content.License" Path="extension/LICENSE.txt" Addressable="true"/>
    <Asset Type="Microsoft.VisualStudio.Services.Icons.Default" Path="extension/images/icon.png" Addressable="true"/>
  </Assets>
</PackageManifest>
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--install", action="store_true", help="install it into VS Code afterwards")
    parser.add_argument("-o", "--output", type=pathlib.Path, help="where to write the .vsix")
    args = parser.parse_args()

    pkg = json.loads((HERE / "package.json").read_text(encoding="utf-8"))
    out = args.output or ROOT / "build" / "vscode" / f"{pkg['name']}-{pkg['version']}.vsix"
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("extension.vsixmanifest", manifest(pkg))
        for name, src in FILES:
            z.write(src, "extension/" + name)
    print(out)

    if args.install:
        code = shutil.which("code") or shutil.which("code.cmd")
        if not code:
            sys.exit("can't find VS Code's `code` command on PATH; install the .vsix from the Extensions view instead")
        subprocess.run([code, "--install-extension", str(out), "--force"], check=True)


if __name__ == "__main__":
    main()
