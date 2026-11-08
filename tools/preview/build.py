import sys
blocks=open("blocks.qml.part").read()
data=sys.argv[1]; openDetail=sys.argv[2]=='1'; out=sys.argv[3]
blocks=blocks.replace('file:///home/root/.local/share/qmarkable/','file://'+data+'/')
blocks=blocks.replace('file:///run/qmarkable/current-doc','file://'+data+'/current-doc')
# harness stubs for firmware-only types
import re
blocks=re.sub(r'Document\.FileType\.Notebook','1',blocks)
blocks=re.sub(r'Document\.FileType\.Pdf','2',blocks)
blocks=re.sub(r'Document\.FileType\.Ebook','3',blocks)
blocks=re.sub(r'BatteryManager\.displayState\s*===\s*BatteryManager\.Normal','root.awake',blocks)
if openDetail:
    blocks=blocks.replace('property  bool detailOpen: false','property  bool detailOpen: true')
src='''import QtQuick
Item {
    id: root
    width: 1404
    height: 1872
    property bool qmHomeVisible: true
    property bool awake: true
    // device font (copy /usr/share/fonts/ttf/noto/NotoSansMono-*.ttf to fonts/NotoSansMono.ttf)
    FontLoader { source: "fonts/NotoSansMono.ttf" }
    FontLoader { source: "fonts/EBGaramond.ttf" }
    FontLoader { source: "fonts/EBGaramond-Italic.ttf" }
    property QtObject passcodeHandler: QtObject { property bool userLocked: false }
    Item {
        id: docStub
        property var document: null
        property bool documentLoaded: false
        signal documentClosed()
    }
    Item {
        id: documentView
        property var item: docStub
    }
'''+blocks+'''
}
'''
open(out,'w').write(src)
