# Pioneer Pro DJ Link & Ableton Link — specifiche operative

**Knowledge base ricavata sul campo**
Versione documento: **2.0** · luglio 2026
Versione precedente: 1.0 · giugno 2026 (sviluppo LUMEN, sola lettura)

> Questo è l'originale italiano, la lingua in cui le misure sono state prese e
> scritte. La traduzione inglese è [`protocol-notes.md`](protocol-notes.md).

---

## 0. Scopo, metodo e affidabilità delle affermazioni

Questo documento raccoglie ciò che è stato decodificato e validato sul campo dei
due protocolli **Pioneer Pro DJ Link** e **Ableton Link**. La versione 1.0
nasceva dallo sviluppo di LUMEN (app audio→DMX), che usa entrambi i protocolli in
**sola lettura** e con requisiti di precisione modesti. Questa versione 2.0 è
stata riscritta durante lo sviluppo di **djbridge**, un bridge bidirezionale su
ESP32 (M5Stack CoreS3-SE) che deve anche **trasmettere** ed essere accettato dai
lettori come tempo master — un requisito molto più severo, che ha costretto a
misurare cose che prima bastava intuire.

Il reverse engineering è **clean-room**: osservazione diretta del traffico con
hardware reale (2× Pioneer **XDJ-700**, **Ableton Live**, **Push 3** standalone).
Dove esistono reimplementazioni di riferimento (beat-link/dysentery di Deep
Symmetry, `ableton-link-rs`) sono state usate **solo per orientarsi**, mai
copiate, e in almeno un caso si è verificato che la loro scelta **non coincide**
con la nostra (§1.5).

### 0.1 Scala di affidabilità — si legga prima di usare il documento

Ogni affermazione non ovvia è marcata. La distinzione è deliberata: in una
revisione precedente alcune ipotesi plausibili erano state scritte come fatti
accertati, e questo ha portato a decisioni di progetto sbagliate. **Un'incertezza
dichiarata vale più di una certezza inventata.**

| Marca | Significato |
|-------|-------------|
| **[M]** | **Misurato** direttamente su hardware reale. Include il riferimento alla cattura o al log. |
| **[D]** | **Dedotto** da misure, con il ragionamento esplicitato. Solido ma non osservato direttamente. |
| **[I]** | **Ipotesi** non verificata. Riportata perché utile, da confermare prima di farci affidamento. |
| **[E]** | Da **fonte esterna** (documentazione di terzi). Non verificato da noi salvo dove indicato. |

La **cattura di riferimento** citata come `[cap07]` è: due XDJ-700, device 1
master e device 2 in sync, 69,7 s, 1430 pacchetti — movimenti di pitch fader sul
master a velocità diverse, poi jog wheel sul master in entrambe le direzioni.

### 0.2 Convenzioni

- Offset di byte **0-based**, in esadecimale (`0x..`).
- **BE** = big-endian, **LE** = little-endian.
- `u8/u16/u32/u64/i64` = interi senza/con segno della larghezza indicata.
- Tempi di Ableton Link in **microsecondi (µs)** salvo diversa nota.

---

## 1. Pioneer Pro DJ Link

Protocollo UDP su rete locale (tipicamente **Ethernet cablata**) con cui i lettori
Pioneer annunciano presenza, stato e beat.

### 1.1 Porte, magic, tipi

| Porta | Uso | Trasporto | Tipo (`0x0a`) | Lunghezza reale [M] |
|-------|-----|-----------|---------------|---------------------|
| **50000** | Keep-alive / announce | broadcast | `0x06` | **54 byte** |
| **50001** | **Beat packet** | broadcast | `0x28` | **96 byte** |
| **50002** | **CDJ status** | broadcast/unicast | `0x0a` | **284 byte** |

**Magic** (primi 10 byte di ogni pacchetto): `51 73 70 74 31 57 6d 4a 4f 4c`
→ `"Qspt1WmJOL"`.

Il **tipo** è il byte a `0x0a`. Il **device number** è a `0x21`.

Le lunghezze in tabella sono quelle osservate sugli XDJ-700 [M, cap07]: 69
keep-alive, 308 beat, 1049 status, tutti della lunghezza indicata senza
eccezioni. Modelli diversi potrebbero usare status più lunghi (dysentery
documenta varianti per CDJ-2000NXS2) — leggere sempre con un controllo di
lunghezza e non assumere.

### 1.2 Keep-alive (porta 50000, tipo `0x06`)

Serve ad annunciarsi e a essere ammessi sulla rete. Layout **validato
byte-per-byte contro un XDJ-700 reale** [M]:

```
0000  51 73 70 74 31 57 6d 4a 4f 4c 06 00 58 44 4a 2d      Qspt1WmJOL..XDJ-
0010  37 30 30 00 00 00 00 00 00 00 00 00 00 00 00 00      700.............
0020  01 02 00 36 01 01 c8 3d fc 09 8f 65 a9 fe 8f 65      ...6...=...e...e
0030  02 00 00 00 01 00                                    ......
```

| Offset | Contenuto |
|--------|-----------|
| `0x0a` | `0x06` = tipo keep-alive |
| `0x0c`–`0x1f` | nome del device, ASCII, zero-padded |
| `0x20`–`0x21` | `01 02` |
| `0x22`–`0x23` | `00 36` = 54, lunghezza totale |
| `0x24` | device number |
| `0x25` | `0x01` |
| `0x26`–`0x2b` | **MAC** dell'interfaccia da cui esce il pacchetto |
| `0x2c`–`0x2f` | **IP** della stessa interfaccia |
| `0x30` | **`0x02`** |
| `0x34` | **`0x01`** |

I byte a `0x30` e `0x34` sono critici: in una prima implementazione si era usato
`0x30=01 0x31=01` e **il lettore non ammetteva il device** [M]. MAC e IP devono
essere quelli dell'interfaccia effettivamente usata (§3.2).

Cadenza di ritrasmissione: **≈1,5 s** [M].

### 1.3 Beat packet (porta 50001, tipo `0x28`, 96 byte)

Emesso **una volta per beat** [M, cap07: 154 pacchetti in 69 s a ~132 BPM = 2,22/s,
esattamente il tempo]. Il campo `beat in bar` cicla `1,2,3,4` [M].

| Offset | Campo | Tipo | Note |
|--------|-------|------|------|
| `0x00`–`0x09` | magic | 10B | `Qspt1WmJOL` |
| `0x0a` | tipo | u8 | `0x28` |
| `0x0b`–`0x1e` | nome device | ASCII | zero-padded |
| `0x1f` | — | u8 | `0x01` |
| `0x20` | subtype | u8 | `0x00` |
| `0x21` | **device number** | u8 | 1–4 |
| `0x22`–`0x23` | len_r | u16 BE | `0x003c` |
| `0x24`–`0x27` | **next beat** | u32 BE | ms al prossimo beat |
| `0x28`–`0x2b` | **2nd beat** | u32 BE | ms al secondo beat |
| `0x2c`–`0x2f` | **next bar** | u32 BE | ms al prossimo downbeat |
| `0x30`–`0x33` | **4th beat** | u32 BE | |
| `0x34`–`0x37` | **2nd bar** | u32 BE | |
| `0x38`–`0x3b` | **8th beat** | u32 BE | |
| `0x3c`–`0x53` | riempimento | 24B | tutti `0xff` [M] |
| `0x54`–`0x57` | **pitch** | u32 BE | `PITCH_UNITY = 0x100000` = 1.0× |
| `0x5a`–`0x5b` | **track BPM** | u16 BE | BPM nominale × 100 |
| `0x5c` | **beat in bar** | u8 | 1–4 |
| `0x5f` | device number | u8 | copia ridondante |

#### Formule dei campi temporali [M, cap07]

Verificate contro i valori reali, con `beat_ms = 60000 / track_bpm`:

```
next beat = beat_ms
2nd beat  = beat_ms × 2
next bar  = beat_ms × (5 - beat_in_bar)
4th beat  = beat_ms × 4
2nd bar   = next_bar + beat_ms × 4
8th beat  = beat_ms × 8
```

Riscontro sul device 1 (traccia 132 BPM, `beat_ms` = 454 ms): la sequenza di
`next bar` osservata è **1363 → 909 → 454** al passare di `beat_in_bar` 2→3→4,
cioè esattamente `454 × (5-bb)`. E `2nd bar` valeva 3181, 2727, 2272, cioè
`next_bar + 1818`. [M]

#### Punto importante e controintuitivo [M]

**`beat_ms` è calcolato sul `track_bpm` NOMINALE, non sul tempo effettivo.**

Il device 2, con traccia a 125 BPM e pitch +5,6% (quindi in riproduzione a 132),
dichiarava `next beat = 480 ms` = 60000/125, **non** 454 ms = 60000/132. [M]

Durante il jog la discrepanza diventa vistosa: il master dichiarava
`next beat = 454 ms` mentre i suoi beat cadevano realmente a **396–441 ms** [M].

**Il campo `pitch` (`0x54`) invece SEGUE il bend del jog**: durante la manovra è
passato da +2,65% a +14,33% e ritorno [M]. Quindi il tempo effettivo istantaneo
si ricava da `track_bpm × pitch`, mentre i campi `next beat` e derivati sono
informativi e non rispecchiano il pitch.

> **[I]** Non sappiamo perché i campi temporali ignorino il pitch. Ipotesi: sono
> pensati per il *quantize* dell'interfaccia utente (allineare un effetto alla
> griglia nominale) e non per la sincronia. Chi vuole sapere quando cade
> davvero il prossimo beat deve calcolarlo da `track_bpm × pitch`, oppure
> — meglio — usare l'**istante di emissione** del pacchetto (§1.7).

### 1.4 CDJ status packet (porta 50002, tipo `0x0a`, 284 byte)

| Offset | Campo | Tipo | Note |
|--------|-------|------|------|
| `0x00`–`0x09` | magic | 10B | |
| `0x0a` | tipo | u8 | `0x0a` |
| `0x21` | **device number** | u8 | 1–4 |
| `0x84`–`0x87` | sync counter | u32 BE | |
| `0x89` | **flag F** | u8 | vedi sotto |
| `0x8c`–`0x8f` | **pitch P1** | u32 BE | vedi §1.5 |
| `0x92`–`0x93` | **track BPM** | u16 BE | × 100 |
| `0x98`–`0x9b` | **pitch P2** | u32 BE | vedi §1.5 |
| `0x9e` | **`Mm`** | u8 | **`2` = traccia NON rekordbox** |
| `0xa0`–`0xa3` | contatore beat | u32 BE | assoluto, crescente |
| `0xa6` | **beat in bar** | u8 | 1–4 |
| `0xc0`–`0xc3` | **pitch P3** | u32 BE | vedi §1.5 |
| `0xc4`–`0xc7` | **pitch P4** | u32 BE | vedi §1.5 |

#### Flag F (`0x89`) [E, verificato in parte]

| Bit | Nome | Significato |
|-----|------|-------------|
| `0x02` | BPM_SYNC | il lettore era in sync ma il DJ ha usato il pitch bend: insegue il tempo ma non allinea più le battute |
| `0x08` | ON_AIR | il canale è aperto sul mixer |
| `0x10` | SYNCED | il lettore è in sync |
| `0x20` | **MASTER** | **è il tempo master** |
| `0x40` | PLAYING | sta riproducendo |

Valori osservati [M, cap07]: master `0xe4` = MASTER+PLAYING; slave in sync
`0xd4` = SYNCED+PLAYING.

Sul bit **BPM_SYNC** abbiamo due osservazioni che concordano:
- in `[cap07]`, dove il jog è stato mosso **sul master**, il bit non compare mai
  su nessuno dei due lettori: lo slave ha seguito senza scollarsi [M];
- in un log precedente, muovendo il jog **sul lettore in sync**, il bit è
  comparso proprio su quello (`0xd4 → 0xd6`) e poi è tornato spento [M].

**[D]** Il bit marca il lettore **le cui battute sono state spostate a mano
rispetto al master**, quindi non compare sul master. Un bridge che voglia sapere
se un lettore è "scollato" può usarlo, ma non deve aspettarselo sul master.

Il contatore a `0xa0` e il `beat in bar` a `0xa6` avanzano insieme e sono
coerenti con i beat packet [M]: chi ascolta i soli status può ricostruire la
posizione nella battuta senza vedere i beat.

**`Mm = 2`** segnala una traccia non analizzata da rekordbox: la beat grid non è
affidabile e il `beat in bar` va ignorato.

### 1.5 I QUATTRO campi di pitch dello status — semantica misurata

Questa è la scoperta più importante della revisione 2.0, e ha risolto un
problema di reattività che era rimasto aperto per giorni.

Lo status contiene **quattro** copie del pitch, a `0x8c`, `0x98`, `0xc0`, `0xc4`
(qui chiamate P1..P4). Le fonti esterne le documentano come "leggermente diverse
in circostanze diverse" senza specificare come [E]. Misurate [M, cap07]:

| situazione | P1 (`0x8c`) | P2 (`0x98`) | P3 (`0xc0`) | P4 (`0xc4`) |
|---|---|---|---|---|
| a riposo | +2,65% | +2,65% | +2,65% | +2,65% |
| movimento del **fader** | tutti e quattro si muovono insieme |
| **jog wheel** (bend) | **+2,65%** | **+18,01%** | **+2,65%** | **+18,01%** |

**Conclusione [D]:**
- **P1 e P3** = pitch **sostenuto** (posizione del fader, o valore imposto dal sync)
- **P2 e P4** = pitch **istantaneo**, bend della jog wheel incluso

Una conferma indipendente e forte: il device 2 aveva il **fader fisico al
centro** ma riportava `P1 = +5,60%` — che con la sua traccia a 125 BPM dà
esattamente 132, il tempo del master [M]. Quindi **P1 non è la posizione fisica
del cursore**: è il pitch che il lettore sta effettivamente sostenendo, che per
un lettore in sync è quello imposto dal master.

> **[I]** Non sappiamo che differenza ci sia fra P1 e P3, né fra P2 e P4: nelle
> nostre catture le coppie sono sempre risultate identiche. Potrebbero
> differenziarsi su altri modelli, o in condizioni che non abbiamo esercitato
> (loop, slip mode, riproduzione inversa).

**Implicazione pratica.** Chi vuole il tempo *effettivo istantaneo* — per esempio
un bridge che deve inseguire anche il pitch bend — deve leggere **P2**. Chi
vuole il tempo *sostenuto* legge P1.

**Divergenza consapevole da beat-link.** La libreria beat-link usa **P1
(`0x8d`)** per `getEffectiveTempo()` [E, verificato leggendo il sorgente].
La scelta è coerente col suo scopo (lì "effective" significa "nominale ×
pitch", in contrapposizione al solo BPM della traccia), ma **non cattura il
bend**. djbridge legge **P2** deliberatamente, ed è ciò che gli ha permesso di
seguire la jog wheel. Chi porta codice dall'una all'altra deve saperlo.

### 1.6 Cadenza di trasmissione — è ADATTIVA [M]

Il dato che ha spiegato la "reattività istantanea" osservata fra due lettori
Pioneer.

**Un CDJ non trasmette a cadenza fissa.** Misurato sul master in `[cap07]`, in
finestre da un secondo:

| condizione | intervallo status | frequenza |
|---|---|---|
| a riposo | **130–165 ms** | ~6,6 Hz |
| **mentre il pitch si muove** | **64 ms** | **~15,6 Hz** |

E il ritorno alla cadenza lenta è **immediato**: a 13 s il lettore stava a 64 ms
con il fader in movimento, a 14 s era già a 136 ms con il fader fermo. Non c'è
alcuna "coda" temporizzata: la cadenza veloce coincide con lo stato *sta
cambiando* [M].

I **beat packet** restano invece a un pacchetto per beat in ogni condizione [M].

### 1.7 Come si comporta un lettore in sync — non insegue, ripete [M]

Misurato confrontando master e slave nello stesso istante:

```
t=55.779   master P2=+9,61%  → 144,69 BPM      slave → 144,69 BPM
t=55.843   master P2=+18,01% → 155,77 BPM      slave → 155,77 BPM
```

Valori **identici alla seconda cifra decimale** [M]. Il lettore in sync calcola

```
pitch_proprio = bpm_effettivo_master / track_bpm_proprio − 1
```

e lo applica. Non c'è un anello di inseguimento né un transitorio.

**Ritardo di reazione misurato: 4–5 ms**, costante, sia con il fader sia con la
jog wheel (20 campioni durante il jog: media 4,3 ms, massimo 5 ms) [M].

**Precisione degli istanti di beat.** Integrando il tempo dichiarato negli status
e verificando dove cadono i beat, il numero di battute fra due beat consecutivi
risulta [M]:

| fase | battute | scarto tipico |
|---|---|---|
| quiete | 1,0000 | 0,33 % |
| movimento fader | 0,9999 | 0,31 % |
| **jog wheel** | **1,0002** | **0,23 %** |

Su una battuta da 440 ms lo 0,3% è **1,3 ms** — e quel valore include il jitter
di rete e di cattura, quindi il jitter proprio del CDJ è **minore di 1,3 ms**,
anche mentre il disco accelera del 18%.

**Allineamento fra i due lettori** (differenza fra gli istanti dei rispettivi
beat packet) [M]:

| fase | media | dispersione | max |
|---|---|---|---|
| quiete | −0,04 ms | 0,82 ms | 2,57 ms |
| fader | +0,05 ms | 2,30 ms | 4,48 ms |
| jog | +0,41 ms | 1,17 ms | 2,32 ms |

Nessuna deriva, nessun recupero progressivo, in nessuna condizione.

> **Conseguenza per chi progetta un bridge.** La prontezza del lettore in sync
> **è la cadenza con cui il master gli parla**. Non c'è nulla da ottimizzare nel
> suo comportamento: i suoi 4–5 ms sono già ottimi. Tutto il ritardo osservabile
> è di chi trasmette. In djbridge, trasmettere status ogni 200 ms fissi
> produceva ritardi di 46–238 ms sui lettori — esattamente la distribuzione
> uniforme dell'attesa del pacchetto successivo [M].

### 1.8 Elezione del master, numero di device, takeover

**Elezione:** il master è il device il cui status ha il bit `0x20` di `0x89`
attivo. In rete c'è al più un master [M].

**Numero di device.** Per la sola lettura si può usare un numero alto (es. 7),
fuori dai 1–4 dei lettori reali. Per **trasmettere** ed essere accettati come
master serve invece un numero **reale (1–4)**, ottenuto con una procedura di
rivendicazione a stadi [M, djbridge]: censimento dei numeri in uso, poi
annuncio ripetuto del numero scelto in più fasi, poi adozione.

**Takeover del master** [M, djbridge]: si negozia attraverso gli status. Un
device che vuole il master lo chiede, il master corrente cede indicando nel
proprio status il numero del successore (campo *master handoff*), e a quel punto
il richiedente assume il ruolo. **La negoziazione passa dagli status**: un device
che smette di trasmetterli mentre è master lascia la rete in uno stallo, perché
nessuno può riprendersi il ruolo. Chi implementa un bridge deve continuare a
trasmettere status anche quando non è master, dichiarando semplicemente il vero.

### 1.9 Trasmettere: cosa serve per essere un master credibile

Sintesi di quanto imparato costruendo djbridge.

**I quattro campi di pitch vanno popolati tutti.** Una prima versione riempiva
solo P2 lasciando P1, P3, P4 a zero: un lettore che legga P1 (come fa beat-link)
ne ricaverebbe `track_bpm × 0 = 0 BPM` [D — il difetto è certo, l'effetto sul
lettore non è stato isolato sperimentalmente]. Non avendo un disco, la scelta
coerente è **pitch unity su tutti e quattro** e tutto il tempo in `track_bpm`.

> Nota sulla semantica: un CDJ dice *"traccia a 132 con pitch +5,6%"*, noi
> diciamo *"traccia a 139,4 con pitch 0%"*. Chi legge moltiplica e ottiene lo
> stesso numero. La differenza è invisibile a valle, e ha il vantaggio di
> rendere `next beat` = 60000/track_bpm anche fisicamente vero — cosa che sul
> lettore reale non è (§1.3).

**Le formule dei campi temporali del beat vanno rispettate** (§1.3): sono state
verificate identiche fra la nostra implementazione e i lettori reali [M].

**La cadenza deve essere adattiva** (§1.6): 150 ms a riposo, ~64 ms mentre il
tempo cambia, con ritorno immediato. E soprattutto, **al cambio di tempo lo
status deve partire subito**, non al prossimo slot periodico.

**L'istante di emissione del beat packet è il riferimento di fase.** I lettori si
allineano a quello, non ai campi del pacchetto (§1.3). Il jitter di emissione si
trasferisce direttamente sulla loro fase.

### 1.10 Metadati della traccia in riproduzione — protocollo `dbserver`

Il beat packet e lo status dicono *quanto veloce* e *dove nella battuta*, ma non
**cosa** sta suonando. Titolo, artista e resto si ottengono interrogando il
lettore che possiede il media, su un protocollo TCP separato.

Le informazioni per individuare la traccia stanno nello **status** del master:
id rekordbox, device che ospita il media, slot e tipo di traccia. Con quelle si
apre il dialogo.

**Sequenza [M, djbridge — validata su XDJ-700]:**

| passo | porta | contenuto |
|---|---|---|
| 1 | TCP **12523** | "centralino": si manda la stringa `RemoteDBServer` preceduta dalla lunghezza (19 byte in tutto: `00 00 00 0f` + `"RemoteDBServer"` + `00`); risponde indicando **su quale porta** servire i dati |
| 2 | TCP **1051** | handshake di 5 byte, che il lettore rimanda in eco |
| 3 | " | **setup** (messaggio `0x0000`): dichiara chi siamo |
| 4 | " | **metadata** (`0x2002`): "quanti elementi hai per la traccia N?" |
| 5 | " | **render** (`0x3000`): "mandameli" → arrivano titolo, artista, tempo |

Sui lettori osservati la porta indicata al passo 1 è sempre risultata **1051**,
la stessa che usano fra loro.

#### Con quale numero di device presentarsi — due vincoli entrambi misurati [M]

Questo è il punto che ha richiesto più tentativi, e nessuno dei due vincoli è
documentato altrove per quanto ne sappiamo.

1. **Deve essere un numero fra 1 e 4.** Presentandosi con un numero alto (7,
   tipico del Virtual CDJ in sola lettura) **il lettore chiude la connessione
   senza rispondere**. Verificato con Wireshark: il pacchetto di setup è byte per
   byte identico a quello di un lettore vero salvo il device number, e la
   risposta è un FIN.

2. **Deve essere un numero PRESENTE IN RETE**, non semplicemente libero.
   Fingendosi il device 3 — libero, ma che nessuno annuncia — il lettore accetta
   il setup ma risponde **"0 elementi"**: non ci conosce e non ha un menu per
   noi. Fingendosi il device 4, che era il nostro e veniva annunciato, risponde
   con **11 elementi** e i metadati arrivano.

**[D]** Il servizio dati sembra quindi mantenere uno stato per device
*annunciato*, e il menu esiste solo per chi è presente sulla rete. La strategia
adottata in djbridge: se il nostro numero è già 1–4 si usa il proprio, altrimenti
si **prende in prestito** il numero di un lettore realmente presente.

#### Quando interrogare

Solo al cambio della coppia **(device master, id traccia)** [M]: così si copre
sia il cambio di traccia sullo stesso lettore sia il passaggio di master, senza
tempestare di richieste. Una risposta con conteggio assurdo (0 o valori
implausibili) va trattata come "riprovare più tardi", non come errore
permanente: capita quando il lettore ha appena caricato e non ha ancora
indicizzato.

---

### 1.11 L'errore "E-8309 Link Access Error" e il portmapper RPC

**Sintomo:** entrambi i lettori mostrano `E-8309 Link Access Error` sul display,
in modo persistente, pur continuando a funzionare.

**Causa, scoperta con Wireshark [M]:** quando un device dichiara nel proprio
status di essere **in riproduzione da uno slot USB**, i lettori vogliono leggerne
le tracce — e per farlo cercano di **montarne il filesystem via NFS**. Inviano
quindi una richiesta **ONC-RPC PORTMAP GETPORT** su **UDP porta 111**, chiedendo
dove risponda il servizio `MOUNT`.

Se il device **tace**, i lettori riprovano **ogni secondo, all'infinito**, e dopo
un po' segnalano l'errore sul display.

**Rimedio [M]:** non serve implementare NFS. Basta **rispondere di no**, in modo
formalmente corretto. Un reply RPC valido con **porta = 0** significa "quel
programma non è registrato qui" ed è sufficiente a far smettere i lettori di
segnalare l'errore.

Struttura della risposta (28 byte, o 24 se non è un GETPORT):

```
XID              4B   identico a quello della richiesta
message type     4B   0x00000001  = REPLY
reply state      4B   0x00000000  = MSG_ACCEPTED
verifier flavor  4B   0x00000000  = AUTH_NULL
verifier length  4B   0x00000000
accept state     4B   0x00000000  = SUCCESS
porta            4B   0x00000000  = non registrato     (solo per GETPORT, proc 3)
```

Si risponde a **GETPORT (proc 3)** e, per cortesia, anche a **NULL (proc 0)**.

> **Il punto generale, che vale oltre questo caso:** sulla rete DJ Link il
> silenzio non è neutro. I lettori interpretano l'assenza di risposta come un
> guasto e insistono; una risposta negativa esplicita chiude la questione. Vale
> anche per le *media query* — le interrogazioni di slot che un lettore manda
> per sapere quali media hanno gli altri, che questo documento non copre in una
> sezione propria — e, sospettiamo, per altri servizi che non abbiamo incontrato.

**Costo implementativo da tenere presente:** il portmapper occupa un socket del
W5500, che ne ha **otto in tutto**. In djbridge è l'ottavo, quindi la dotazione è
satura: qualunque servizio aggiuntivo richiederà di liberarne uno o di
multiplexare. [M]

**Alternativa non verificata [I]:** se un bridge **non dichiarasse** di avere un
media in riproduzione da USB, forse i lettori non chiederebbero affatto il
mount, rendendo il portmapper superfluo. Non l'abbiamo provato perché dichiarare
la riproduzione serve ad altro (un master fermo non trascina la fase degli
altri).

---

## 2. Ableton Link

Protocollo UDP peer-to-peer su rete locale (tipicamente **WiFi**). Non c'è un
master: tutti i peer condividono una **timeline** comune. Due fasi: **discovery**
(multicast) e **measurement** (unicast).

### 2.1 Discovery — multicast `224.76.78.75:20808`, magic `_asdp_v`

**Header (20 byte):**

| Offset | Campo | Tipo | Note |
|--------|-------|------|------|
| `0x00`–`0x06` | magic | 7B | `_asdp_v` |
| `0x07` | versione | u8 | `0x01` |
| `0x08` | message type | u8 | `0x01`=ALIVE, `0x02`=RESPONSE, `0x03`=BYEBYE |
| `0x09` | TTL | u8 | tipicamente `0x05` |
| `0x0a`–`0x0b` | reserved | 2B | zero |
| `0x0c`–`0x13` | peer id | 8B | **effimero**, vedi §2.6 |

**Payload: TLV** (`key` 4 ASCII + `len` u32 BE + `value`). Chiavi note:

| Key | Len | Contenuto |
|-----|-----|-----------|
| `tmln` | 24 | **timeline**: 3× u64 BE = `tempo` (µs/beat), `beatOrigin` (microbeat), `timeOrigin` (µs ghost) |
| `sess` | 8 | **session id** (= peer id del founder) |
| `stst` | 17 | **start/stop state** |
| `mep4` | 6 | **measurement endpoint** IPv4: `IP`(4B) + `porta`(u16 BE) |

Note:
- Il **RESPONSE (`0x02`)** è inviato **unicast** (invisibile a uno sniffer solo
  multicast). Esiste anche uno scambio di stato unicast (~107 byte) con lo
  **stesso formato** dell'ALIVE.
- Un secondo canale multicast su **porta 20909** (magic `ablsd_v`, payload con
  `local.transfer`) è **Ableton-specifico ed estraneo al clock** → ignorare.

**Campo `stst` (17 byte):**

| Offset nel valore | Campo | Tipo |
|---|---|---|
| 0 | `playing` | u8 (`0x01`=play) |
| 1–8 | `beats` | i64 BE (microbeat) |
| 9–16 | `timestamp` | i64 BE (µs) |

Lo stato di transport è **condiviso di sessione**. La **timeline è continua** e
avanza sempre, anche a transport fermo: tempo/beat/fase si ricavano solo dalla
timeline.

### 2.2 Measurement — unicast verso il `mep4`, magic `_link_v`

**Header (9 byte):** magic `_link_v` (7B) + versione `0x01` + message type
(`0x01`=PING, `0x02`=PONG). Poi TLV con valori **u64**:

**PING** (misuratore → `mep4`): `__ht` = host time del misuratore (µs);
`_pgt` = prev ghost time, **assente nel primo ping**.
→ primo PING 25 byte, successivi 41.

**PONG** (peer → porta sorgente): `sess`, `__gt` (ghost time del responder),
`__ht` (**eco**, per appaiare), `_pgt` (eco se presente).
→ primo PONG 57 byte, successivi 73.

Il measurement è **bidirezionale**. Per un peer passivo basta la direzione
attiva. Il responder risponde a chiunque gli mandi un ping sul `mep4`.

### 2.3 GhostXForm

Per ogni coppia ping→pong appaiata tramite l'eco di `__ht`:

```
host_mid = (host_send + host_recv) / 2
offset   = ghost_resp − host_mid
```

Si tiene una finestra mobile (es. 16 campioni) e se ne prende la **mediana**
(robusta agli outlier). `ghost(now) = host(now) + offset_mediano`.

La pendenza host↔ghost è ≈1.0 (entrambi clock real-time in µs): il drift su
scala di secondi è trascurabile e l'offset è ri-stimato di continuo, quindi la
mediana basta. Servono ~4 campioni per agganciare.

### 2.4 Phase math

```
bpm       = 60_000_000 / tempo
ghost_now = host_now_µs + offset_mediano
beats     = beatOrigin/1e6 + (ghost_now − timeOrigin) / tempo
phase     = beats mod quantum
```

`beatOrigin` è in **microbeat**. Per la fase nel beat usare `mod 1.0`, per quella
nella battuta `mod 4`.

### 2.5 Fusione di sessione

Quando due peer si scoprono **convergono su un'unica `session id`**: uno vince e
l'altro adotta la sua.

> **[I]** Il criterio di vittoria non è stato verificato da noi. Osservato una
> volta: ha prevalso la sessione del Push, con Live che ha abbandonato la propria
> dopo pochi pacchetti [M, singolo episodio]. La documentazione di Link parla di
> un timestamp di fondazione con tie-break sull'id [E]. **Per un peer che non
> vuole imporre il proprio tempo la regola pratica è adottare la sessione che
> prevale.**

### 2.6 peer id effimeri

Il `peer id` (e quindi la `session id` del founder) è **rigenerato a ogni
sessione**: non è un identificatore stabile del dispositivo. Usarlo solo per
raggruppare i peer nella sessione corrente, mai come identità persistente.

### 2.7 Propagazione del tempo

Un cambio di tempo si propaga **in tempo reale a tutti i peer**: riportano lo
stesso `tempo` praticamente nello stesso istante [M].

**Verifica indipendente [M, djbridge]:** campionando il tempo della sessione a
20 Hz durante cambi imposti da Ableton Live, ogni cambio è risultato un **gradino
singolo** — mai una rampa. Cinque cambi su cinque: `0 ms, 1 gradino`. La
risoluzione della misura è però 50 ms, quindi l'affermazione corretta è *"non
rampa in modo percepibile a 20 Hz"*, non *"salta istantaneamente"*.

### 2.8 Uso della libreria ufficiale (`abl_link`, C API)

Per chi usa la libreria invece di reimplementare [E, dalla documentazione
dell'header, verificato leggendo il sorgente]:

- **`abl_link_create(double bpm)` richiede un tempo iniziale**: non esiste un
  costruttore senza. Ma l'istanza è **inizialmente disabilitata** e non comunica
  in rete, quindi quel tempo non viene imposto a nessuno finché non si abilita.
- L'imposizione avviene solo con un **commit** dello stato di sessione: la
  documentazione dice esplicitamente che lo stato committato **sostituisce**
  quello della sessione e viene comunicato agli altri peer.
- **Non esiste una modalità "partecipo ma non propongo"**: un'istanza abilitata
  ha comunque una sua timeline che entra nella fusione.

**[D]** Conseguenza pratica per un bridge che deve poter stare in rete senza
imporre nulla: non basta astenersi dal chiamare `set_tempo`, bisogna verificare
che **nessun percorso di codice** faccia commit. In djbridge un percorso di
svuotamento differito sfuggiva al controllo e consegnava il tempo a valle dello
spegnimento del ponte [M].

---

## 3. Lezioni di rete (costate molto debug)

### 3.1 Firewall / inbound UDP

*Little Snitch* (macOS) blocca **silenziosamente** l'UDP **in ingresso** verso
applicazioni non firmate, **senza mostrare alcun avviso**. Conseguenze osservate:
keep-alive DJ Link ricevuti = 0; measurement Link che non aggancia. Vale per
**ogni** peer, anche Ableton Live. Soluzione: regola inbound esplicita per le
porte usate (DJ Link 50000–50002; Link 20808 + measurement) oppure firmare il
bundle.

**Il traffico da terminale (`python`, `tcpdump`) passa comunque**: lo sniffer
vede tutto e l'app no, il che rende la diagnosi molto confusa.

### 3.2 Selezione dell'interfaccia (multi-homed)

Con più interfacce (WiFi + LAN), **non** scegliere per routing (su macOS la rotta
link-local `169.254/16` finisce sul WiFi) né per solo match di sottorete
(ambiguo con più link-local). Criterio corretto: identificare **da quale
interfaccia ARRIVANO** i pacchetti, con **`IP_RECVIF`** (macOS/BSD) o
**`IP_PKTINFO`** (Linux) + `recvmsg()` + `if_indextoname()`.

### 3.3 WiFi e client isolation

Il measurement di Link è **unicast peer-to-peer**. Con la *client isolation*
attiva sull'AP il multicast di discovery passa ma l'unicast fra client viene
bloccato → i peer si vedono ma **non si fondono**. Va disabilitata.

### 3.4 Sniffing dell'unicast

Per catturare il measurement con `tcpdump` la macchina sniffer deve essere **un
capo** della conversazione (su WiFi un terzo client non vede l'unicast altrui
nemmeno in promiscuous).

### 3.5 Hardware

Il **Push 3 standalone è WiFi-only**; gli XDJ-700 parlano DJ Link sul cablato. In
un setup misto **i due protocolli stanno su reti diverse**, e il bridge deve
stare su entrambe.

---

## 4. Implementazione su ESP32 — lezioni da djbridge

### 4.1 Parsing e memoria

Pacchetti piccoli (≤284 byte), parsing a offset fissi (DJ Link) o TLV lineare
(Link). **Nessuna allocazione dinamica** necessaria.

**Memoria interna: è la risorsa scarsa.** Con un peer Ableton Link connesso
l'heap interno di un CoreS3 è sceso a **13 KB** [M]. Voci principali misurate:

| voce | prima | dopo | note |
|---|---|---|---|
| buffer di render LVGL (2×) | 51 200 B | 30 720 B | 40 → 24 righe |
| stack task LVGL | 16 384 B | 8 192 B | ne usava 4 800 |

Recupero ottenuto: **+22 KB** di heap interno [M]. I buffer LVGL devono stare in
RAM interna (il flush li manda in DMA): **non** si possono spostare in PSRAM.

### 4.2 Timing e clock

- Host time in µs: **`esp_timer_get_time()`** (int64 monotonico).
- Il timer software è **puntuale entro ~120 µs** [M]; il ritardo che conta è
  quello fra il risveglio e il byte sul filo.

### 4.3 Contesa del bus SPI — il problema principale

Su CoreS3 il **W5500 (rete), il display e la SD condividono lo stesso bus SPI**,
e il pin MISO è anche il DC del display. Conseguenze misurate [M]:

- un flush del display teneva il bus fino a **10,7 ms** con buffer da 40 righe,
  **6,5 ms** con 24 righe;
- una transazione di rete può tenerlo fino a **18 ms**;
- il beat packet, che è il riferimento di fase per i lettori, usciva fino a
  **8,6 ms** dopo il risveglio del timer.

**Rimedio adottato:** una *finestra di silenzio* attorno all'istante del beat, in
cui i consumatori del bus si astengono. Va fatta rispettare a **tutti**: in una
prima versione la rispettava solo il display, e i drenaggi di rete continuavano a
prendere il bus. Dopo averla estesa a tutti, l'uscita tipica è scesa da ~7,5 ms
a ~2,9 ms [M].

**Limite noto:** la finestra impedisce di *iniziare* una transazione, non
accorcia quelle già in corso. Resta un picco residuo a ~7,4 ms [M], attribuibile
a una transazione lunga avviata poco prima dell'apertura.

> Per confronto: il jitter dei CDJ reali è **sotto 1,3 ms** in ogni condizione
> (§1.7). È il metro da usare.

### 4.4 Deriva dei quarzi

Il quarzo del CoreS3 e quello del lettore non coincidono. Misurato [M]:

- deriva residua tipica: **13–24 µs/s** (≈20 ppm);
- **il quarzo si scalda**: partendo da freddo, il rapporto fra i due clock si è
  spostato di **~20 ppm nei primi cinque minuti**, con costante di tempo ~130 s.

**[D]** Una costante di calibrazione memorizzata non è quindi una soluzione: il
valore giusto a macchina fredda è sbagliato dopo dieci minuti. Serve una
correzione **online**.

**Approccio che ha funzionato** [M]: retroazione sulla **pendenza** della
deviazione di fase (non sul suo valore assoluto), con finestra di misura fissa da
60 s, più un termine lento di recupero dell'offset accumulato. Risultato: RMS
della deviazione **0,89 ms**, nessun riaggancio forzato in oltre 6 minuti.

**Trappola documentata:** una media mobile o una EMA sulla deviazione **non
funziona** in presenza di riagganci di fase. Ogni riaggancio azzera la
deviazione, quindi introduce un **gradino** nella serie; una regressione lineare
lo interpreta come pendenza. Per un gradino `h` a metà di una finestra `T`, la
pendenza spuria vale circa `1.5·h/T` — con h=4 ms e T=30 s fa **200 ppm** contro
una deriva vera di 20 [D, verificato in simulazione]. La strada corretta, se si
vuole una finestra continua, è **ricostruire la serie** sommando i salti che si
sono applicati (che sono noti esattamente).

### 4.5 Reattività alle discontinuità note

**Principio emerso a caro prezzo:** quando una discontinuità è un **fatto
comunicato** (il pacchetto di status dice che il BPM è cambiato), filtrarla è
sbagliato. Un filtro passa-basso sulla fase esiste per lisciare il *jitter di
misura*; applicato a un cambio noto, fa inseguire per secondi un valore già
disponibile.

In djbridge il rimedio corretto si è rivelato **ri-ancorare** la stima di fase al
dato fresco, esattamente come si faceva già per i salti da hot cue — non
modificare il meccanismo di aggancio. Tre tentativi precedenti (α variabile,
sospensione del forcing, riallineamento differito) hanno tutti fallito perché
agivano sul meccanismo invece che sull'informazione che gli veniva data [M].

---

## 5. Questioni aperte

Elencate perché **non** diventino certezze per omissione.

1. **Perché i campi temporali del beat packet ignorano il pitch** (§1.3).
   Misurato con certezza, non spiegato. Ipotesi: servono al quantize della UI.

2. **Differenza fra P1 e P3, e fra P2 e P4** (§1.5). Nelle nostre catture le
   coppie sono sempre identiche. Non abbiamo esercitato loop, slip mode,
   riproduzione inversa.

3. **Criterio di fusione delle sessioni Link** (§2.5). Un solo episodio
   osservato, documentazione esterna non verificata.

4. **Un lettore lento a inseguire, occasionalmente.** In due prove distinte un
   XDJ-700 ha impiegato **~2 secondi** invece dei consueti 50–200 ms per
   raggiungere il tempo imposto, mentre l'altro arrivava in 74 ms [M]. È
   accaduto una volta verso l'alto e una verso il basso, quindi **non** è il
   limite del campo di corsa del pitch. Causa ignota: potrebbe dipendere dal
   lettore, dalla traccia, o dallo stato in cui si trovava.

5. **Ritardo fra beat packet e uscita audio.** Tutte le nostre misure di fase
   confrontano *pacchetti* con *pacchetti*. Se un lettore avesse un ritardo
   sistematico fra il beat packet e l'audio che esce dalle sue uscite, non lo
   vedremmo. Servirebbe un riferimento acustico o un'analisi del segnale.

6. **Tenuta della compensazione di deriva attraverso un cambio di master.**
   La scelta implementata è *conservare* la stima (è un rapporto fra quarzi, e
   il quarzo nostro non cambia). Argomentata ma **mai osservata in funzione**:
   in tutte le sessioni di prova il correttore era ancora in fase di misura.

---

## 6. Riferimenti di codice

**LUMEN** (Python, sola lettura, stdlib pura):
- `djlink.py` — parsing beat/status, Virtual CDJ keep-alive, selezione
  interfaccia via `IP_RECVIF`/`IP_PKTINFO`, selezione master.
- `link.py` — Ableton Link passivo + measurement, GhostXForm a mediana.

**djbridge** (C, ESP-IDF, bidirezionale) — questo repository:
- `components/djlink/` — parsing e **costruzione** di keep-alive, beat, status.
- `main/main.c` — macchina a stati di rete (censimento → rivendicazione →
  osservatore → master), compensazione di deriva, motore dei beat in
  trasmissione, finestra di silenzio sul bus SPI.
- `components/linkbridge/` — involucro sulla libreria ufficiale `abl_link`.

---

*Documento rivisto luglio 2026. Le affermazioni marcate **[M]** sono state
misurate su hardware reale; quelle **[D]** dedotte da misure con ragionamento
esplicitato; quelle **[I]** sono ipotesi dichiarate come tali; quelle **[E]**
provengono da fonti esterne e non sono state verificate salvo dove indicato.*
