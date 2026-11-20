"""Transforme Web/page.html en Appli/Core/Inc/web_page.h (page servie par la carte).
A relancer apres chaque modification de page.html :  python Web/generer_page.py"""
import os
ici = os.path.dirname(os.path.abspath(__file__))
html = open(os.path.join(ici, "page.html"), encoding="utf-8").read()
data = html.encode("utf-8")
lignes = []
for ligne in data.split(b"\n"):
    s = ligne.decode("utf-8").replace("\\", "\\\\").replace('"', '\\"')
    lignes.append('  "' + s + '\\n"')
c = ("/* web_page.h - GENERE par Web/generer_page.py a partir de Web/page.html : ne pas modifier */\n"
     "#ifndef WEB_PAGE_H\n#define WEB_PAGE_H\n\n"
     "static const char WEB_PAGE_HTML[] =\n" + "\n".join(lignes) + ";\n\n#endif /* WEB_PAGE_H */\n")
open(os.path.join(ici, "..", "Appli", "Core", "Inc", "web_page.h"), "w", encoding="utf-8").write(c)
print("web_page.h genere :", len(data), "octets")
