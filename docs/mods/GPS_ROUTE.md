# Mod: trasa GPS na radarze — rozpoznanie (2026-11-14)

**Stan: wersja 1 działa** (`src/game/vcs_gps.cpp`, gałąź `mod/gps-route`). Ustawienie: F2 → Enhancements → Radar →
GPS route (Off / In a vehicle / Always; `VCS_GPS=0/1/2`). Sprawdzone w ukrytym teście (build `build/gps`, misja 44
JER_A2, teleporty wzdłuż trasy): linia jest pod blipami i strzałką, przycięta do koła radaru, w kolorze blipa
(fioletowa dla koloru 5), a trasa skraca się 110 → 75 → 46 → 22 węzłów. Komenda `gps` zwraca cel, liczbę węzłów
i liczbę czworokątów.

**Wersja 2** (ten sam dzień): dwie trasy naraz, do celu misji i do własnego waypointu. Waypoint stawia i usuwa prawy
klik na mapie w pauzie (`vcs::gps::mapClick` z `vcs_frontend.cpp`; poza mapą prawy klik dalej działa jako "wstecz").
Waypoint to nasz własny coord blip w tablicy radaru (kolor RGBA `0x3CA0FFFF`), więc gra sama rysuje go na radarze
i mapie, a zapis gry go zachowuje. Znika po dojechaniu na 20 m. Trasy rysują się też na mapie w pauzie: `0x113418`
woła też `FUN_0026BA60` (rysowanie mapy) z `radar+0x1AD9 = 1`. `FUN_00111130` ustawia wtedy środek (0,0), zasięg
1000 i obrót 0, a `FUN_00114758` liczy pozycję z `+0x1ADC/+0x1AE0` (przesunięcie mapy) i `+0x1AE4` (zoom) × 0.56 ×
(4/3 albo 1 dla TV 16:9) wokół (-0.4256, -0.1166). Sprawdzone: obie trasy na radarze i mapie, waypoint ląduje pod
kursorem, a klik na nim go usuwa.
- **Mapa przesuwa blipy punktowe**, gdy `radar+0x1AF0` (vec3, wejście do wnętrza) ≠ 0 (`FUN_001112C0`, 0x1114E4...).
  W testach rig-a było tak po teleporcie z wnętrza koszar bez przejścia przez drzwi, a w normalnej grze na zewnątrz
  pole jest zerowe. Trasy i tak chowają się we wnętrzach.
- Blipy aut i postaci: pozycja we wpisie zostaje (0,0), radar czyta encję na żywo. "Skaczące" blipy to najpewniej
  zaokrąglanie HUD do pikseli 640×448 (`FUN_003E0368` robi cvt.w.s), niezwiązane z modem.

**Wersja 3**: własny waypoint usunięty, bo gra ma swój **player target**. Na mapie w pauzie stawia go kwadrat
(Spacja na klawiaturze), pole pada +0x26. `FUN_0026BE00` woła wtedy `FUN_00115030` = SetTargetBlip: typ 4, kolor 0,
**ikona +0x29 = 0x10**, flagi |0x10, display 2. Mod szuka blipa z ikoną 0x10 i rysuje do niego czerwoną trasę nad
trasą misji. Prawy klik w menu znowu działa jak "wstecz".
- **Węzły wodne**: tablica węzłów aut zawiera też trasy łodzi, oznaczone **flagą +8 bit 0x100** (z = 6, poziom
  morza). To całe osobne składowe grafu wzdłuż wybrzeża (składowe 1, 2, 4, 5, 6, 7), a główna sieć dróg nie ma tej
  flagi wcale. Bez filtra pierwsza misja nowej gry (cel (-1824, -716) w Fort Baxter) prowadziła pętlą po wodzie, bo
  "najbliższy węzeł auta" był wodny. Po filtrze: 37 węzłów lądem.

- **Drogi piętrowe** (2026-11-15): najbliższy węzeł do gracza i do celu szukany w 3D. Wysokość węzła to s8 pod +4
  (pełne metry). Różnica do 2 m jest ignorowana, ponad to ×10. Bez wysokości (target z mapy ma z = 0) liczy się
  tylko x, y. Cel pierwszej misji (-1823.9, -715.9, z 14.3) stoi na górnym pokładzie Fort Baxter. Tylko po x, y
  wygrywał węzeł 2406 na dolnej drodze (z 6, 10 m obok), a teraz wygrywa 1035 (z 13, 50 m obok) i trasa kończy się
  rampą 9 → 11 → 13. W sieci aut jest 20 par węzłów nad sobą (≤ 12 m w poziomie, ≥ 5 m w pionie), głównie Fort
  Baxter i okolice (-800, 0).

Nie sprawdzono jeszcze: jazdy autem (tryb "In a vehicle"), blipów na aucie lub postaci (uchwyt → pula), mapy w
pauzie (trasa powinna się chować), wnętrz, widescreenu 16:9, znaczenia górnych bitów linków (jednokierunkowe?).
Pomysły zaczerpnięte (bez kodu; repo nie ma licencji) z GTA-GPS-Redux dla SA.

## 0. Potwierdzone w działającej grze (2026-11-14, ukryty rig z kopią zapisu po "Soldier", PS2X_AUDIO=0)

Zrzuty leżą w `build/analysis/gps/` (nodes.bin, links.bin, paths_obj.bin). Skrypty sondujące były tymczasowe.

- **Radar**: `*0x48F548` = 0x74E800. Zasięg `+0x1AC8` = 30.0 m (pieszo). Centrum `0x4D3E68` = pozycja gracza
  (x, y). `0x4D3E70` / `0x4D3E74` = sin / cos (0.350 / 0.937). Zgadza się z analizą statyczną.
- **Blipy**: układ wpisu potwierdzony. ADD_BLIP_FOR_COORD ze skryptu rig-a trafił do wolnego slotu 4,
  uchwyt `0x20004` = indeks | (licznik << 16). Pozycja z z = -100 ląduje na ziemi.
  - Blipy z ikoną (kontakty, kryjówki): `+0x28 = 01`, **`+0x29` = id ikony (8, 0x0A, 0x2B...)**, flagi 0x7E / 0x76.
  - **Blip celu misji** (JER_A2, mission 44): kolor 5, typ 4 (coord), **`+0x29 = 0` (bez ikony)**, `+0x28 = 03`,
    flagi 0x66 (bity 3 / 4 zgaszone). Heurystyka celu GPS: blip w użyciu z `+0x29 == 0`.
  - Typ 5 = blip ikony dla współrzędnej / punktu kontaktowego (kolor 2).
- **ThePaths** `*0x487490` = 0x1A44D70, nagłówek:
  `+0x00` węzły, `+0x04` / `+0x08` dwie inne tablice (?), **`+0x0C` = 8387 węzłów łącznie,
  `+0x10` = 3079 węzłów dla aut (indeksy 0..3078), `+0x14` = 5308 dla pieszych (3079..8386)**,
  `+0x18` f32 1952.0 (?), **`+0x7BA0` = wskaźnik linków** (+0x7BA4 kolejna tablica, +0x7BA8 = 100, +0x7BAC = 37).
- **Cała sieć jest w pamięci naraz.** Wskaźnik i nagłówek nie zmieniają się po teleportach przez wyspy
  (-400,1000 i 500,-500). Nie ma streamowania stref.
- Układ węzła (10 bajtów) potwierdzony. Graf jest symetryczny (0 linków jednostronnych), linki aut nigdy nie
  prowadzą do węzłów pieszych, wszystkie indeksy są poprawne. Auta: x -2080..1501, y -1838..1817.
- Górne 2 bity linku: 0 (17320×), 2 (56×), 3 (276×). Znaczenie nieznane (jednokierunkowe? światła?
  zamknięte mosty na początku gry?).
- Graf aut ma 13 spójnych składowych: 2391 węzłów w głównej, potem 266, 185, 125, 28... Mniejsze to
  prawdopodobnie odcięte fragmenty (wyspy, parkingi). Przy celu w innej składowej trzeba prowadzić do
  najbliższego osiągalnego węzła.
- **Test A\***: najbliższy węzeł do gracza (2439 @ -1656,-317,13) **zgadza się z GET_CLOSEST_CAR_NODE gry**.
  Trasa z Fort Baxter do celu misji (-711.5, -1541.1) to 110 węzłów, 2000 m (w linii prostej 1545 m),
  362 rozwinięte węzły, czyli ułamek milisekundy.

Wszystko poniżej pochodzi z **analizy statycznej** (listing asemblera z komentarzy w `generated/ee/*.cpp`
oraz tablica opcode'ów odczytana z `build/elf/SLUS_215.90`). Nic nie zostało jeszcze sprawdzone w działającej
grze: znaczenie pól oznaczonych "?" trzeba potwierdzić zrzutem RAM (rig, `VCS_DATA_DIR` na kopii zapisów).

Numery komend skryptowych VCS pochodzą z Sanny Builder Library (`vcs/vcs.json`). Handlery odczytano z tablicy
opcode'ów pod `0x4BEAF0` (8 bajtów na wpis: s16 delta, u16 valid, u32 handler).

## 1. Blipy (CRadar)

Obiekt radaru: **`*(u32*)0x48F548`** (wskaźnik; `lui 0x49` + `-0xAB8`). Gdy jest zerem, gra wywołuje
`func_471A48` (leniwe tworzenie).

| Komenda | Opcode | Handler | Co woła |
|---|---|---|---|
| REMOVE_BLIP | 0x00BB | 0x35B038 | `func_1153B0(radar, handle)` = ClearBlip |
| ADD_BLIP_FOR_CAR / CHAR | 0x00C0 / 0x00C1 | 0x35B380 / 0x35B498 | |
| ADD_BLIP_FOR_COORD | 0x00C3 | 0x35B6C8 | `func_114F00(radar, type 4, &pos, colour 5, display 3)` → handle, potem `func_115590(radar, handle, 3)` |
| CHANGE_BLIP_DISPLAY | 0x00C4 | 0x35B840 | |
| ADD_SPRITE_BLIP_FOR_COORD | 0x00C6 | 0x35BA30 | |

ADD_BLIP_FOR_COORD z z = -100 szuka ziemi (`func_196778(x, y)`), tak jak w VC.

**`func_114F00(radar, type, pos*, colour, display)` = SetCoordBlip.** Szuka wolnego slotu w tablicy
**`radar + 0x280`: 75 (0x4B) wpisów po 0x30 bajtów**. Slot jest zajęty, gdy bit 2 w słowie flag jest
ustawiony. Uchwyt zwraca `func_110E30(radar, index)`, a przy braku wolnego slotu wynik to -1.

Wpis (offset od początku wpisu):

| Offset | Typ | Co |
|---|---|---|
| +0x00 | u32 | kolor (argument `colour`; 5 dla coord blipa ze skryptu) |
| +0x04 | u32 | typ (4 = coord; car / char / object / pickup / sprite to inne wartości, do potwierdzenia) |
| +0x08 | u32 | uchwyt encji (0 dla coord) |
| +0x10 | vec4 | pozycja w świecie (x, y, z, w) |
| +0x20 | u32 flagi | bit 1 ustawiony przy tworzeniu, **bit 2 = w użyciu**, bity 0 / 3 / 4 czyszczone, **bity 5..7 = display** (`display & 7`) |
| +0x24 | f32 | skala (1.0) |
| +0x28 | u8 | 1 przy tworzeniu (?) |
| +0x29 | u8 | 0 (?) |

Pozycja blipa pojazdu lub postaci jest zapewne odświeżana z encji przez uchwyt w +0x08 (do potwierdzenia).
Do celu GPS wystarczy jednak pozycja z +0x10 po klatce gry albo rozwiązanie encji.

## 2. Sieć dróg (ThePaths)

Obiekt: **`*(u32*)0x487490`** (wskaźnik). Podany handler jest dla GET_CLOSEST_CAR_NODE (0x01B5, handler 0x36CAF0):

- `func_268880(paths, pos*, type, a3=1, t0=1, 0, 0, 0, f12=800.0)` = FindNodeClosestToCoors, zwraca indeks
  węzła albo -1. `type` (a2): 0 dla aut (GET_CLOSEST_CAR_NODE). Szuka po siatce sektorów w `paths + 0x670`
  (wiersz 0x258 bajtów, komórka 12 bajtów, typ * 4) i `paths + 0x678`.
- `func_26B200(out vec4*, paths, node)` = pozycja węzła (z przesunięciem na pas, gdy węzeł ma szerokość).

**Węzeł: 10 bajtów**, tablica `*(paths + 0x0)`, liczba węzłów (lub maksymalny indeks) w `paths + 0xC`:

| Offset | Typ | Co |
|---|---|---|
| +0 | s16 | x × 8 (pozycja = wartość × 0.125) |
| +2 | s16 | y × 8 |
| +4 | s8 | z w metrach; gdy < 0, z = wartość − 100 (tak robi `func_26B200`) |
| +5 | u8 | szerokość / przesunięcie pasa (× 0.0625 + 2.5) |
| +6 | s16 | indeks pierwszego połączenia w tablicy linków |
| +8 | u16 | flagi; **`& 0xF` = liczba połączeń**; `>> 12 & 3` = klasa drogi (game-internals.md 12d, spawner) |

**Linki**: `*(paths + 0x7BA0)`, u16 na wpis, **`& 0x3FFF` = indeks sąsiedniego węzła** (górne 2 bity to
flagi, prawdopodobnie kierunek lub ruch jednokierunkowy; do sprawdzenia).

Do zrobienia: czy pod `paths + 0x4` / `+0x10` leży osobna tablica węzłów dla pieszych (argument `type`); czy
węzły są w pamięci w całości, czy streamowane strefami (VCS przełącza "levele" między wyspami:
game-internals.md 12). Trzeba też sprawdzić, czy wszystkie wyspy są w tablicy naraz.

## 3. Radar: świat → radar → ekran

- **`func_114C98(radar, out vec2*, in vec2*, f12 scale)` = TransformRealWorldPointToRadarSpace**:
  `v = in × scale − centre`, gdzie centre to `0x4D3E68` (vec2; przy scale ≠ 1 przesunięte o (−1.5, −5)).
  Dalej `v /= range`, gdzie range to `radar + 0x1AC8` (f32, zasięg radaru w metrach, zależny od prędkości).
  Na końcu obrót: `cos = *0x4D3E74`, `sin = *0x4D3E70`,
  `out.x = cos·v.x + sin·v.y`, `out.y = −sin·v.x + cos·v.y`.
  **Wynik: radar to okrąg jednostkowy** (|out| ≤ 1 = w środku radaru).
- **`func_114758(radar, out screen*, in radar*)` = TransformRadarPointToScreenSpace**: dla radaru w HUD
  (`radar + 0x1AD9 == 0`) `x = rx + rw/2 + in.x · rw/2`, `y = ry + rh/2 − in.y · rh/2`. Prostokąt radaru
  pochodzi z obiektu HUD `*(u32*)0x48F374`: `func_31F2D0` = x, `func_31F270` = szerokość, `func_31F2E0` = y,
  `func_31F2C0` = wysokość. Na mapie w pauzie (`+0x1AD9 != 0`) obowiązuje inna gałąź: środek `+0x1ADC/+0x1AE0`,
  zoom `+0x1AE4`, stałe 240 / 114 i 4/3 bez widescreenu.
- `func_114978` = odwrotność dla mapy w pauzie (ekran → radar), przydatna przy stawianiu własnego waypointu.

## 4. Kolejność rysowania

`0x321500` (radar w CHud): `func_110EB0(radar)` rysuje kafelki mapy, a `func_113418(radar)` strzałkę gracza,
północ i blipy (wewnątrz `TheCamera` = `0x6F44D0`, `func_114C98` ×5, `func_114758` ×3). Rysowanie idzie przez
RwIm2D (`FUN_002fbe08`, wierzchołek 16 bajtów: game-internals.md 6f).

## 5. Plan moda (wersja 1: trasa do celu misji)

1. Hook na wejściu `func_113418` (przed oryginałem, AGENTS.md reguła 2): narysować trasę, potem wywołać
   oryginał, żeby blipy i strzałka leżały nad linią.
2. Cel: aktywny blip z tablicy `radar + 0x280`. Wybór: pierwszy blip misji (kolor lub display do ustalenia),
   później własny waypoint.
3. Trasa: najbliższy węzeł do gracza i do celu (własne przeszukanie tablicy albo `func_268880`), potem A* po
   węzłach i linkach na hoście, przeliczane co ~0.5 s albo po zjechaniu z trasy.
4. Rysowanie: punkty trasy przez `func_114C98`, przycięcie odcinków do okręgu jednostkowego w przestrzeni radaru
   (matematycznie, bez sztuczek z głębią), potem `func_114758` i pasek z quadów RwIm2D.
5. Opcja w ustawieniach (F2), domyślnie wyłączona (projekt: "authentic by default").

## Odcinki poza drogą (przez ściany)

Linia to: gracz → węzły dróg → cel. Pierwszy i ostatni odcinek są proste, bez sieci dróg, więc gdy cel stoi
za murem (np. na ogrodzonym placu), linia szła prosto przez ściany. Teraz te odcinki rysuje się tylko, gdy mają
najwyżej `kOffRoad` = 12 m. Dłuższe są pomijane, a trasa kończy się (lub zaczyna) na drodze.

## DIRTY FIX: ręczne końce tras

`kDirtyFixRouteEnds` w `vcs_gps.cpp` (funkcja `dirtyFixMissionSoldierRouteEnd`): gdy blip misji stoi w promieniu
`radius` od podanego punktu, trasa prowadzi do ręcznie wybranego punktu (sam blip się nie zmienia).
- Misja 1 "Soldier", "Go to your barracks": blip ok. (-1648.87, -261.00) → koniec trasy (-1733.14, -314.18, z 14.71).
  Sprawdzone w teście: blip wstawiony przez ADD_BLIP_FOR_COORD (0x00C3) daje `mission=-1733.1,-314.2 nodes=4`.
