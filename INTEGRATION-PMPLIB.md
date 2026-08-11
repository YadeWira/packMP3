# packMP3 — guía de integración de la librería (`pmplib_*`)

Para consumidores externos que quieran embeber packMP3 vía `dlopen`/link
directo. Escrita para PPDF (packPDF), aplica igual a cualquier otro.

Estado verificado: **packMP3 v3.0d (08/10/2026)**.

---

## 1. Artefactos y cómo construirlos

```sh
cd /home/forum/git/packMP3/source
make clean          # importante, ver "rough edge" abajo
make so             # -> source/libpackMP3.so   (Linux x86-64)
make dll-x64        # -> source/bin/packMP3.dll (Windows x64, mingw, estático)
make dll-x86        # -> source/bin/packMP3.dll (Windows x86)
make lib            # -> source/libpackMP3.a    (estática, si preferís linkear)
```

Artefacto ya construido y listo para usar:

```
/home/forum/git/packMP3/source/libpackMP3.so      (352 KB)
/home/forum/git/packMP3/source/packmp3dll.h       <- usá ESTE header
/home/forum/git/packMP3/source/packmp3lib.h       (interno del build, no el tuyo)
```

**Nota de Windows**: si cross-compilás vos, usá los drivers `-posix`
(`x86_64-w64-mingw32-g++-posix`). No es preferencia: la `libpackJPG.a`
vendorizada está construida contra winpthreads y linkearla en un binario
con thread model win32 da un deadlock en runtime, sin ningún error de
link. Aplica a cualquiera que linkee estas librerías estáticamente, no
sólo a packMP3. Ver la sección del README.

(El viejo rough edge de `make clean` entre targets está arreglado desde
v3.0d: cada variante tiene su propio directorio de objetos.)

---

## 2. API (6 símbolos, `extern "C"`)

```c
void        pmplib_init_streams( void* in_src, int in_type, int in_size,
                                 void* out_dest, int out_type );
bool        pmplib_convert_stream2mem( unsigned char** out_file,
                                       unsigned int* out_size, char* msg );
bool        pmplib_convert_stream2stream( char* msg );
bool        pmplib_convert_file2file( char* in, char* out, char* msg );
const char* pmplib_version_info( void );
const char* pmplib_short_name( void );
```

`in_type` / `out_type`:

| valor | significado                                            |
|-------|--------------------------------------------------------|
| `0`   | archivo — el puntero es un `char*` con el nombre        |
| `1`   | memoria — el puntero es el buffer (`in_size` = tamaño)  |
| `2`   | `FILE*` (p.ej. `stdin`/`stdout`) — `in_size` se ignora  |

Para salida a memoria, pasá `out_dest = NULL, out_type = 1`; el buffer lo
devuelve `pmplib_convert_stream2mem` por `out_file`/`out_size`.

`msg` es un buffer del llamador; reservá **1024 bytes** (`MSG_SIZE`). Se
escribe tanto en éxito ("Compressed to PMP in memory (83.15%) in 201ms")
como en error.

### Uso mem→mem (el caso de packPDF)

```c
unsigned char* out = NULL; unsigned int outlen = 0; char msg[1024] = {0};

pmplib_init_streams(in_buf, 1, (int) in_len, NULL, 1);
if (!pmplib_convert_stream2mem(&out, &outlen, msg)) {
    /* fallo limpio: msg dice por qué; no hay salida que liberar */
} else {
    /* usar out[0..outlen) ... */
    free(out);                    /* <-- OWNERSHIP: es tuyo, ver §3 */
}
```

### Dirección: **automática, no hay flag**

`pmplib_init_streams` mira los **2 primeros bytes** del input:

- `"MS"` (`0x4D 0x53`) → es un archivo `.pm3` → **descomprime**
- cualquier otra cosa → asume MP3 → **comprime**

No hay parámetro para forzar dirección desde la API. Si querés detectar el
tipo vos mismo antes de llamar, el magic `MS` es todo lo que necesitás.

> Nota de nomenclatura: la **extensión en disco es `.pm3`**
> (`pmp_ext = "pm3"`, packmp3.cpp:486). "PMP" sobrevive sólo como nombre
> interno de variables y en los mensajes de la propia lib ("Compressed to
> PMP in memory ..."). No existe la extensión `.pmp`.

---

## 3. Ownership de memoria (importante)

`pmplib_convert_stream2mem` devuelve el buffer interno del writer y **le
transfiere la propiedad al llamador**: internamente `getptr()` pone
`fmem = false`, así que el destructor del stream ya **no** lo libera.

- El buffer sale de `malloc`/`realloc` → liberalo con **`free()`** plano.
- Si la conversión **falla** (`return false`), `*out_file`/`*out_size` **no
  se tocan**. Inicializalos en `NULL`/`0` y no liberes nada.
- No hay función `pmplib_free()`; `free()` es correcto y es la única opción.

---

## 4. Alcance: qué SÍ y qué NO hace la librería

Esto es lo más importante y no es obvio desde el header. El build de
librería (`-DBUILD_LIB` / `-DBUILD_DLL`) **excluye deliberadamente** varias
cosas que sí tiene el CLI:

| capacidad                                    | CLI | lib/DLL |
|----------------------------------------------|-----|---------|
| MPEG-1/2/2.5 **Layer III** (mp3)             | ✔   | ✔       |
| **Layer I/II** (mp1/mp2, backend packMP2)    | ✔   | ✘       |
| Recompresión de **carátulas ID3v2** (APIC) con packJPG / packPNG | ✔ | ✘ |
| Contenedor **chunked** `-k` (magic `"MK"`)   | ✔   | ✘ (*)   |
| Archivos **M2** (Layer I/II, magic propio)   | ✔   | ✘       |

(*) el código de chunking se compila en la lib, pero la API no expone
ningún setter para activarlo, y el detector de `pmplib_init_streams` sólo
conoce el magic `"MS"`.

Un `.pm3` chunked (`"MK"`) o un archivo M2 (`"M2"`) pasado a la lib falla
**limpio** — `false`, sin buffer de salida, nunca salida corrupta
silenciosa. **Desde v3.0d el mensaje además nombra la causa real**:

| entrada | mensaje |
|---|---|
| chunked (`"MK"`) | `chunked archive (-k): supported by the packMP3 CLI, not by the library API` |
| M2 (`"M2"`) | `MPEG Layer I/II archive: supported by the packMP3 CLI, not by the library API` |
| `.mp2` crudo | `file is MPEG-1 LAYER II, not supported` |
| basura | `no mpeg audio data recognized` |

Hasta v3.0c los dos primeros decían `"no mpeg audio data recognized"`, que
era engañoso: son archivos packMP3 perfectamente válidos, sólo que en un
contenedor que este build no puede abrir.

Chequear el magic de tu lado sigue siendo razonable si querés interceptar
el caso antes de llamar, pero ya no hace falta para no confundir a tu
usuario: la lib dice la verdad.

Los tags ID3v2/ID3v1 sí se preservan byte-exactos en la lib (van al coder
genérico); lo que falta es sólo la *recompresión* de la carátula.

Consecuencia práctica para packPDF: si el stream de audio embebido no es
Layer III, la lib te va a devolver `false` con
`"no mpeg audio data recognized"`. Es un fallo **limpio**, no un crash.

---

## 5. Errores

`pmplib_convert_stream2mem` devuelve `false` y llena `msg`. Verificado con
basura de 4 KB: `rc=0, msg="no mpeg audio data recognized"`, sin crash y
sin buffer que liberar.

Con `out_type = 0` (salida a archivo) y error, la lib **borra** el archivo
de salida a medio escribir. Con salida a memoria no hay nada que limpiar.

---

## 6. Thread-safety

Todo el estado mutable por archivo es `static thread_local`
(`#define THREAD_LOCAL static thread_local`), sin guardas de `BUILD_LIB`, o
sea que aplica igual en la lib.

- **Un hilo puede hacer una conversión a la vez.** El par
  `init_streams` + `convert_*` usa estado de hilo: no intercales dos
  conversiones en el mismo hilo.
- **Varios hilos en paralelo: OK, sin mutex.**

Verificado hoy con la `.so` por `dlopen`: 8 hilos × 4 iteraciones
(comprimir + descomprimir mem→mem, con `free()` de ambos buffers en cada
vuelta) → salida **byte-idéntica** a la serial en las 32 corridas, sin
crash ni corrupción.

No necesitás serializar `pmplib_*` como sí hacés con `pjglib_*`.

---

## 7. Requisitos de plataforma

`.so` Linux x86-64, dependencias sólo del sistema:

```
libstdc++.so.6, libgcc_s.so.1, libc.so.6, libm.so.6
```

Pisos de símbolos versionados:

- **GLIBC_2.14**
- **GLIBCXX_3.4.22** (⇒ libstdc++ de GCC 5.1+)
- **CXXABI_1.3.9**

No arrastra `libz` / `liblzma` / `libzstd`: esas son dependencias de
packPNG, que está fuera del build de librería (§4).

Exporta exactamente los 6 símbolos `pmplib_*` (`-fvisibility=hidden`), así
que **no hay riesgo de colisión ODR** con tu copia de packJPG ni con nada
más — a diferencia del caso `.a`, la `.so` no expone `model_s`/`model_b`
ni ningún símbolo del coder.

> Si en cambio linkeás la **estática** `libpackMP3.a`, ahí sí exporta los
> símbolos internos del coder (`model_s`/`model_b`, etc.) y podés chocar
> con packJPG igual que me pasó a mí. Para `dlopen` esto no aplica.

Windows: `make dll-x64` / `dll-x86` producen una DLL estáticamente
enlazada (`-static-libgcc -static-libstdc++`), sin dependencias de runtime
de mingw. **No damos soporte a XP.**

---

## 8. Formato del archivo `.pm3`

- Extensión en disco: **`.pm3`** (no `.pmp` — ver §2). La usan tanto los
  archivos Layer III (`"MS"`) como los M2 de Layer I/II (`"M2"`); los
  distingue el magic, no el nombre.
- Magic: `"MS"` (2 bytes).
- `appversion` actual: **31** (stamp de compatibilidad de archivo, separado
  de la versión visible 3.0c).
- Un binario viejo con `appversion` menor **rechaza limpiamente** un
  archivo más nuevo (chequeo `hcode > appversion`), no lo malinterpreta.
- `appversion_legacy_min` = 20: archivos desde v2.0 en adelante siguen
  decodificando.

Si packPDF guarda `.pm3` dentro del PDF, tené en cuenta que la garantía de
reconstrucción byte-exacta es **de packMP3 sobre el stream MP3 original**;
no metas transformaciones intermedias (recorte de tags, normalización)
entre el stream del PDF y la lib.

---

Dudas / bugs: escribime por `aim dm PMP3`.
