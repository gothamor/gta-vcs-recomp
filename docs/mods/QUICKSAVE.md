# Mod: szybki zapis (quick save) pod klawiszem (2026-11-15)

**Stan: działa** (`src/game/vcs_quicksave.cpp`).

**Ustawienia:**
- F2 → Enhancements → Saving → **Quick save**: Off / Enabled / Enabled + prompt after mission, domyślnie **Off**;
  `VCS_QUICKSAVE=0/1/2`.
  `VCS_QUICKSAVE_HOOKS=0` nie instaluje haków.
- Klawisz: F2 → Keyboard → Menus → **Quick save**, domyślnie **F12**. Gra nie używała F12; diagnostyka renderera
  (`PS2X_HW_DUMP_KEY=1`) też, ale F12 to zrzut ekranu w Steamie i Bandicamie, dlatego klawisz da się zmienić.

**Jak działa:**
- Klawisz otwiera **menu zapisu gry** (wybór slotu albo Cancel) w dowolnym miejscu poza misją, pieszo albo w
  pojeździe.
- Zapis to normalny zapis gry: te same sloty, ten sam format pliku. Wczytuje się z Load Game albo sam przy
  starcie, a późniejszy zwykły zapis w kryjówce jest znowu zwykły.
- Po wczytaniu jesteś **tam, gdzie zapisałeś**:
  - w tym samym kierunku,
  - we wnętrzu kryjówki albo budynku Empire, jeśli tam stałeś,
  - z tym samym pojazdem (siedzisz w nim albo stoi tam, gdzie go zostawiłeś).
- **Zegar się nie przesuwa** (zapis w kryjówce przesuwa go o 6 h).
- **Enabled + prompt after mission**: to samo menu otwiera się też samo po zaliczonej misji, żeby nie zapomnieć
  zapisać. Hak na handlerze `REGISTER_MISSION_PASSED` (opcode 0x01EB, `FUN_00184D18`) zaznacza zaliczenie, a menu
  otwiera się, gdy „Mission passed” jest na ekranie od 5 s, skrypt misji skończył się 1,5 s temu, kamera jest na
  graczu i gra nie zapisuje. Zapis jest w miejscu końca misji. Gdy wcześniej zacznie się następna misja albo gra
  zostanie wczytana, menu się nie otwiera. Test: komenda `quicksave prompt` (jak zaliczenie misji).

Ten mod zastąpił „autozapis po misji” (jego historia jest niżej).

## Kiedy klawisz nic nie robi (komunikat na ekranie)

- opcja wyłączona: „Quick save is off (Settings > Enhancements > Saving)”,
- trwa misja: u8 `0x4CD3E7`. Zlecenia poboczne (Paramedic, Vigilante, straż, taksówka, wyścigi) gra też uruchamia
  jako misje, więc też się liczą,
- przerywnik albo kamera skryptowa (`TheCamera+0x888` ≠ 1), gracz martwy, gra już zapisuje,
- **wnętrze, którego gra nie umie odtworzyć**, czyli obszar ≠ 0, a gracz nie jest ani w kryjówce (G171), ani w
  budynku Empire (G172), np. sklep. Nic by go tam z powrotem nie wstawiło.
- otwarte menu ustawień albo trenera, pauza.

## Zapis

1. **Procedura zapisu z kryjówki**: główny skrypt, IP **0xC646**. Każdy punkt zapisu woła ją przez CALL
   (0x1222B). Mod uruchamia ją jako nowy skrypt maszyny skryptów gry (`vcs::rig::startScriptAt`, powrót na
   `TERMINATE_THIS_SCRIPT` pod 0x16E90) z **L0 = 1**, bo wtedy sama rozjaśnia ekran (0xC71B). Procedura:
   - wyłącza sterowanie,
   - ustawia **G4 = G684 = 1**: flagi, po których skrypt `INITSAV` rozpoznaje po wczytaniu zapis z kryjówki,
   - zapamiętuje miejsce startu (G678..680 → G174..176), broń (0xC4C3) i pieniądze,
   - ściemnia ekran,
   - wywołuje `ACTIVATE_SAVE_MENU` i czeka na `HAS_SAVE_GAME_FINISHED`,
   - przywraca kamerę, rozjaśnia ekran i zeruje flagi.

   Sam opcode `ACTIVATE_SAVE_MENU` (pierwszy autozapis) zostawiał G4 = 0, więc taki zapis wczytywał się jak nowa
   gra.
2. **W pojeździe**: handler `ACTIVATE_SAVE_MENU` gry (`FUN_002D38C8`) w pojeździe nic nie robi
   (`FUN_00219D50`). Dla skryptu moda handler jest podmieniony i robi to, co gra robi pieszo: `0x48A65C = 1`
   (żądanie menu) i licznik `0x4CD480 + 1`. Auto najpierw staje (`SET_CAR_FORWARD_SPEED 0`), bo procedura zabiera
   sterowanie.
3. **Zegar**: menedżer karty tuż przed zapisaniem slotu (0x275EC4) woła `FUN_003ADF68`, a ta
   `FUN_0024D5B8(0x168)`, czyli +360 minut („sen” w kryjówce). Na czas szybkiego zapisu hak na `FUN_0024D5B8`
   pomija wywołanie z 360. 720 minut po śmierci lub aresztowaniu (0x24C778, 0x24CB7C) zostaje bez zmian.
4. **Dane szybkiego zapisu** trafiają tylko do pliku. Wpisuję je tuż przed zapisem, a zaraz po nim gra dostaje z
   powrotem swoje wartości.

## Gdzie są dane szybkiego zapisu (i dlaczego tam)

**Zapis gry przechowuje ze stanu skryptów tylko zmienne globalne, i to nie wszystkie.**
- Funkcja wczytująca blok skryptów (`FUN_0025A348`, z ogólnego wczytywania `FUN_003ADAE0`) kopiuje globalne
  słowo po słowie.
- Słowo przywraca tylko wtedy, gdy jego offset jest na liście w segmencie 2 MAIN.SCM: 1092 offsety u16, lista,
  którą przeszukuje `FUN_00255188`. W game-internals.md 11a/11b była błędnie opisana jako tabela modeli.
- Pozostałe globalne są przy wczytaniu **zerowane**.
- Działające skrypty nie są zapisywane: każdy startuje od nowa, stąd `INITSAV` i flaga G4.

Sprawdziłem to tak:
- dane w zmiennych lokalnych skryptu i w zmiennych LAN_B1 (G6346..) były w pliku, ale po wczytaniu znikały,
- G1106 i G678 przetrwały.

Dlatego dane są w miejscach, które gra sama zapisuje:

- **Miejsce gracza: G678..680 (punkt startu) i G177 (kierunek)**, czyli tam, gdzie gra trzyma punkt startu
  kryjówki. Po wczytaniu `INITSAV` (główny skrypt 0x2ADF7) **sam** stawia tam gracza:
  - G171 ≠ −1: wchodzi do wnętrza kryjówki (0x20C69),
  - G172 ≠ −1: wchodzi do budynku Empire (0x14750, 0x126D0),
  - w pozostałych przypadkach gracz jest na zewnątrz: `LOAD_SCENE`, potem `SET_CHAR_COORDINATES`, z kierunkiem G177.

  Na czas zapisu G678..680 / G177 = bieżące miejsce. Gdy gracz jest na zewnątrz, G171 i G172 = −1 (mogą zostać
  z wcześniejszego wnętrza: po teleporcie G171 nadal wynosiło 0). Po zapisie gra dostaje z powrotem swoje
  wartości. Po wczytaniu szybkiego zapisu G678..680 trzyma jego miejsce, dopóki kolejny zapis go nie nadpisze.
  Czytają je tylko zapis i `INITSAV`.
- **Reszta: G1106, jedno słowo.** Z 1092 przywracanych zmiennych to jedyna, której nie używa żaden skrypt
  (dezasembler MAIN.SCM: odwołania i tablice z rozmiarami) i która była 0 we wszystkich obejrzanych zapisach.

  | bity | znaczenie |
  |---|---|
  | 31..28 | 0xB: szybki zapis (0xA: pierwsze szybkie zapisy, bez danych w ułamku G177) |
  | 27..26 | pojazd: 0 brak, 1 gracz w nim siedzi, 2 zaparkowany obok (ostatni prowadzony, istnieje, ≤ 100 m) |
  | 25..17 | model pojazdu |
  | 16..9 / 8..1 | miejsce zaparkowanego pojazdu względem gracza, ze znakiem, jednostki 0,8 m (±100 m) |
  | 0 | wolny |

  **Ułamek G177.** Kierunek gracza jest zapisywany z dokładnością do pełnego stopnia, a ułamek (15 bitów,
  wartość / 32768, dokładnie dla kierunku < 512) niesie:

  | bity | znaczenie |
  |---|---|
  | 14..12 | gwiazdki (ped +0x920, 0..6); po wczytaniu `ALTER_WANTED_LEVEL_NO_DROP` |
  | 11..7 | wysokość zaparkowanego pojazdu względem gracza, jednostki 0,8 m, 16 = ta sama, 0 = poza ±12 m |
  | 6..0 | kierunek zaparkowanego pojazdu (360° / 128) |

  Po wczytaniu, gdy `INITSAV` skończy (G4 = G684 = 0), mod czyta G1106, zeruje je, na zewnątrz stawia gracza
  jeszcze raz (`LOAD_SCENE`, `SET_CHAR_COORDINATES`: przy starcie gry `INITSAV` stawiał go, zanim ziemia się
  wczytała, i spadał pod mapę), oddaje gwiazdki i odtwarza pojazd: `REQUEST_MODEL`, `CREATE_CAR`,
  `WARP_CHAR_INTO_CAR`, `MARK_CAR_AS_NO_LONGER_NEEDED`.

  **Wysokość pojazdu.** `CREATE_CAR` podnosi podane z o wysokość modelu nad jego środkiem (auto ok. 1 m, motor
  0,6 m), więc z ma być ziemią. G680 po wczytaniu nie jest już zapisaną wysokością (w aucie o 1,8 m niżej:
  auto stworzone na tej wysokości lądowało pod drogą i spadało pod mapę), dlatego wysokości liczę od miejsca, w
  którym gracz stoi po wczytaniu (jego środek 1 m nad ziemią):
  - gracz w aucie: `CREATE_CAR` na z gracza − 1,
  - auto zaparkowane: `CREATE_CAR` na z = −100 (najwyższa ziemia w tym miejscu). Gdy wynik różni się od zapisanej
    wysokości (z gracza + różnica z ułamka G177) o 0,6 m lub więcej (most nad autem, dach, ziemia jeszcze
    niewczytana), mod ustawia autu zapisaną wysokość.
- **Czego nie da się zapisać**: kolorów i stanu technicznego pojazdu. W zapisie gry nie ma na nie wolnego
  miejsca, więc auto wraca jako ten sam model, w kolorze wybranym przez grę.

Ostatni pojazd: pole peda (+0x450/+0x480/+0x484) zeruje się przy wyjściu z auta, więc mod sam pamięta ostatnie
auto gracza. Przy zapisie sprawdza, czy auto nadal jest w puli: wpis zajęty, ten sam model, odległość ≤ 100 m.

## Wnętrza i budynki Empire (poprawka błędu z AUTOSAVE.md)

Wcześniejszy opis mówił, że budynki Empire „nie mają wnętrza do wczytania”. **To było błędne.** Budynki Empire
mają wnętrza z dyskiem zapisu, tak jak kryjówki:
- skrypt punktu zapisu wchodzi do budynku (0x12676..: `CALL 0x11131(1)`, G2301, **G172 = numer budynku 0..29**),
  a przy wyjściu ustawia `CALL 0x11131(0)`, G172 = −1 (0x127A3..),
- dane budynków są w tablicach GA1583/1613/1643 (położenie), GA263, GA323, GA353, GA413 (0x14750),
- po wczytaniu `INITSAV` przy G172 ≠ −1 woła 0x14750 (G172), stawia gracza w GA1583..[G172] i woła 0x126D0
  (G172, 0),
- dla kryjówek to samo robi G171 (0..3: koszary, Viceport, (−1073, 353), (255, −177); wnętrza 22 + i).

Szybki zapis w kryjówce albo w budynku Empire zachowuje G171/G172, więc wczytanie przechodzi przez tę samą ścieżkę
gry co zwykły zapis tam, z dokładną pozycją z G678..680.

Stare zapisy z autozapisu (wersja 2 robiła je tam, gdzie skończyła się misja): przy wczytaniu, jeśli G171 = G172 =
−1, G1106 nie jest szybkim zapisem, a G678..679 to punkt startu jednej z 4 kryjówek (GA3305/3309), mod wchodzi do
niej (0x20C69 z L0 = i) i stawia gracza w jej punkcie startu. Dla budynków Empire takiej poprawki nie ma: punkt
startu Empire (G2293.. z obiektu dysku, 0x119B0) nie pokrywa się z żadną tablicą, a karty z takim zapisem nie
miałem. Taki stary zapis wczytuje się na ulicy. Wystarczy wejść do budynku i zapisać ponownie.

## Co sprawdzono

Ukryte kopie gry, `tools/quicksave_check.py`, port 47701, kopia karty `build/rig/saves/user`, build
`build/autosave`. 13/13:

| Test | Wynik |
|---|---|
| ustawienie, haki | ok |
| we wnętrzu koszar (G171 = 0, obszar 2) → slot 2 | menu, zapis, ekran wraca |
| pieszo w Vice Point (470, 900) → slot 3 | zegar 16:09 → 16:15 (minuty procedury, bez +6 h); haczyk zegara zadziałał |
| w aucie jadącym 15 m/s → slot 4 | menu w aucie (auto zatrzymane) |
| pieszo 12 m od auta, z którego wysiadł → slot 5 | zapisane jako „zaparkowane obok” |
| wczytanie slotu 3 z menu Load Game | dokładnie (470,0, 900,0, 13,3), kierunek, obszar 0, zegar bez skoku |
| wczytanie slotu 4 | gracz **w aucie** tego samego modelu (211) |
| wczytanie slotu 5 | gracz pieszo, auto modelu 211 w zapisanym miejscu (≤ 3 m) |
| wczytanie slotu 2 | we wnętrzu koszar (obszar 2, G171 = 0), ta sama pozycja i kierunek |
| potem zwykły zapis z kryjówki (kod punktu zapisu 0x12213, `quicksave savepoint`) → slot 6 | ok |
| klawisz podczas misji (44) | odmowa, bez menu |
| start gry z kartą bez slotu 6 (najnowszy = szybki zapis 5) | wczytany sam przy starcie, w zapisanym miejscu |
| start z kartą ze slotem 6 (zwykły zapis) | koszary jak zawsze, bez przywracania szybkiego zapisu |

Uwaga do testów: po wczytaniu z menu ScriptSpace jest pod innym adresem, więc zmienne trzeba czytać przez wskaźnik
`0x48741C`.

## Nie sprawdzono

- **Budynku Empire** (brak karty z kupionym budynkiem). Ścieżka gry jest ta sama co dla kryjówek (G172 /
  `INITSAV`), ale nie była uruchomiona.
- Kryjówek 2 i 3, motocykla, łodzi, helikoptera i samolotu. Pojazd w powietrzu zapisuje się w powietrzu, więc po
  wczytaniu spadnie.
- Gry z kontrolerem: klawisz jest tylko na klawiaturze, bez skrótu na padzie.

## Historia: autozapis po misji (wersje 1-2, zastąpione)

- v1: sam `ACTIVATE_SAVE_MENU` po `REGISTER_MISSION_PASSED`. Zapis wczytywał się jak nowa gra (G4 = 0) i ze złym
  strojem.
- v2: procedura 0xC646. Z L0 = 0 zostawał czarny ekran „Loading...”, bo przy L0 = 0 ekran rozjaśnia dopiero
  wołający skrypt. W aucie gra odmawiała, a zapis odpalał się przy znaczniku następnej misji. Wczytanie stawiało
  gracza poza wnętrzem kryjówki.

Z tych wersji zostały: procedura zapisu (L0 = 1), obejście blokady w pojeździe i poprawka wczytania starych
zapisów z autozapisu w kryjówkach.
