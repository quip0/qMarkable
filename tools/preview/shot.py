import sys, os
from PySide6.QtCore import QUrl, QTimer
from PySide6.QtGui import QGuiApplication
from PySide6.QtQuick import QQuickView
app=QGuiApplication(sys.argv)
v=QQuickView(); v.setSource(QUrl.fromLocalFile(sys.argv[1]))
errs=v.errors()
for e in errs: print('ERR',e.toString())
if errs: sys.exit(1)
v.resize(1404,1872); v.show()
def grab():
    v.grabWindow().save(sys.argv[2]); app.quit()
QTimer.singleShot(1500, grab)
app.exec()
