# Diseño: integridad declarada en el contenedor `.pm3`

Documento de diseño, **sin código**. Todos los números están medidos hoy sobre
`master` (42 commits sobre `v3.0f`) con `tests/corruption.sh`; los comandos
para rehacerlos están al lado de cada tabla.

---

## 1. Qué está roto exactamente

Un `.pm3` dañado puede decodificar a un MP3 **distinto del original, con exit 0
y sin ninguna señal**. No es que falten guardas: el archivo dañado es una
codificación *válida* de otro mp3, así que no hay nada inconsistente que un
chequeo interno pueda encontrar.

Son **dos clases distintas**, y esto importa porque **cada opción cierra una
sola**:

| clase | cómo se produce | medido |
|---|---|---|
| **A — truncación** | faltan bytes al final del archivo | 16 de 72 celdas (corpus de 12 archivos, cortes de 1–13 bytes) |
| **B — corrupción in situ** | un bit dado vuelta, el largo no cambia | 14 de 646 celdas de la suite; **las 14 son de un solo bitflip** |

La mayoría de la clase A produce salida **del tamaño exacto del original** con
bytes distintos, que es el peor caso posible: ni el tamaño sirve de señal.

> Corrección respecto de lo que decía `DECISIONES-PENDIENTES.md` hasta hoy: yo
> había escrito que el residuo eran "archivos truncados". Medido por régimen,
> las 14 de la grilla de la suite son **todas de bitflip**, ninguna de
> truncación. Las dos clases existen, pero el residuo que la suite pinea es la
> clase B.

---

## 2. Lo que ya funciona, dentro del formato actual

`packMP3` tiene **tres contenedores**, y uno de ellos ya declara largos:

| contenedor | prefijo | payload |
|---|---|---|
| `MS` Layer III normal | 11 bytes (23 con carátula) | stream aritmético **sin delimitar**, hasta EOF |
| `MK` chunked (`-k<n>`) | `2 + 1 + 1 + 4*K` | K sub-streams `MS`, **cada uno con su largo declarado** |
| `M2` Layer I/II | 4 bytes | delegado a packMP2, hasta EOF |

**`MK` es la prueba de existencia de que el largo declarado cierra la clase A**,
y ya está en el binario que se publica. Mismos archivos, mismos cortes:

| archivo | `MS` | `MK` |
|---|---|---|
| `ff_128.mp3` | 1 incorrecta | **0** |
| `ff_320.mp3` | 1 incorrecta | **0** |
| `lame_cbr_160.mp3` | 2 incorrectas | **0** |

Y el otro lado, sobre 200 bitflips del mismo archivo:

| contenedor | silenciosamente incorrectas |
|---|---|
| `MS` | 24 de 200 |
| `MK` | **17 de 200** |

O sea: el largo declarado **cierra la clase A y no la B** — la reduce (24→17,
porque un flip en la tabla de largos sí se detecta) pero deja 17 vivas.

---

## 3. Las tres opciones

### Opción 1 — largo de payload declarado

Escribir el largo del stream en la cabecera. Al decodificar, exigir que el
archivo tenga exactamente ese tamaño.

- **Cierra**: clase A, completa. Demostrado por `MK`.
- **No cierra**: clase B. Un bit dado vuelta no cambia el largo.
- **Costo**: 4 bytes. Rechaza **antes** de decodificar, que es barato.
- **Nota**: para `MS` es un cambio de formato; para `MK` ya existe.

### Opción 2 — checksum del payload comprimido

Un CRC32/xxhash del stream, en la cabecera o al final.

- **Cierra**: A y B, en cuanto a **integridad del archivo**.
- **No cierra**: un error del propio códec. Si el `.pm3` está intacto pero el
  decodificador tiene un bug, el checksum coincide y la salida es incorrecta.
- **Costo**: 4–8 bytes y una pasada sobre el payload.

### Opción 3 — checksum del MP3 original *(recomendada)*

Guardar el hash del archivo de entrada. Al descomprimir, hashear la salida y
comparar.

- **Cierra**: A, B, **y cualquier causa de salida incorrecta**, incluido un bug
  del códec o una diferencia entre versiones del decodificador. Es lo único que
  verifica la propiedad que el programa promete —*losslessness*— y no un proxy
  de ella.
- **Costo**: 4–8 bytes, y una pasada sobre la salida. No permite rechazo
  temprano: hay que decodificar entero para saber. Eso es aceptable porque de
  todos modos se iba a decodificar.
- **Efecto secundario útil**: convierte `-ver` en una verificación de
  **integridad** y no solo de consistencia, que es el punto menor #3 de
  `DECISIONES-PENDIENTES.md`.

**Recomendación: opción 3**, y opción 1 *además* si se quiere rechazo temprano
en archivos grandes. No son excluyentes: el largo ahorra decodificar un archivo
que ya se sabe roto, el hash de salida garantiza el resto.

---

## 4. Dónde va, contenedor por contenedor

- **`MS`**: campo nuevo en la cabecera, después del byte de versión. La
  alternativa de agregarlo al final **no sirve**: el stream aritmético se lee
  hasta EOF, así que un checksum al final se consumiría como datos del stream.
- **`MK`**: ya tiene la tabla de largos. El hash de salida va en la cabecera del
  contenedor, sobre el mp3 concatenado, no por chunk.
- **`M2`**: mismo lugar que `MS`. El payload lo produce packMP2, así que el hash
  de salida es la única de las tres opciones que se puede aplicar sin tocar la
  librería hermana.

---

## 5. Compatibilidad hacia atrás — **ya está resuelta**

El gate de versión ya hace exactamente lo que un cambio de formato necesita.
En `uncompress_pmp`:

```c
if ( hcode != appversion && hcode < appversion_legacy_min ) → "incompatible file"
if ( hcode > appversion )                                   → "file from a newer
                                                               build; upgrade to decode"
```

Con `appversion = 31` y `appversion_legacy_min = 20`:

- Un binario **viejo** leyendo un archivo **nuevo** (`appversion` 32): entra por
  el segundo chequeo y **rechaza limpio**, con un mensaje que dice qué hacer.
  No hay lectura silenciosa de un campo que no entiende.
- Un binario **nuevo** leyendo un archivo **viejo** (20..31): pasa, y
  `pmp_archive_version` le dice que el campo no está. Los archivos existentes se
  siguen leyendo.

**Entonces el plan es**: subir `appversion` a 32 y gatear el campo nuevo con
`pmp_archive_version >= 32`. Mismo precedente que los bits de versión MPEG en
v1.3 y que el registro APIC en v2.1.

Lo que **sí** rompe: archivos nuevos no los leen binarios viejos. Eso es
inevitable y es el costo real de la decisión.

---

## 6. Lo que **no** cierra

- Un `.pm3` al que alguien le recalcule el checksum a propósito. Esto es
  detección de corrupción, no autenticación; no hay defensa contra un archivo
  hecho a medida.
- Nada del camino de **compresión**. Los 2 casos de "comprime y después no se
  puede descomprimir" (punto menor de `DECISIONES-PENDIENTES.md`) son otro
  problema: el encoder emite valores que el decoder rechaza, y un checksum no
  lo toca.

---

## 7. Costo de no hacerlo

La suite **pinea el 14** para que no crezca en silencio. Sin cambio de formato,
esas 14 celdas siguen: un `.pm3` con un bit dado vuelta puede entregar un MP3
distinto del que se comprimió, sin decir nada. Para una herramienta cuyo
contrato es *losslessness*, esa es la falla que más importa y la única que no se
puede cerrar desde adentro.

---

## 8. Cómo rehacer estos números

```sh
cd source && make test                 # grilla completa, 646 celdas
make corrupt-asan                      # lo mismo bajo ASAN+UBSan
PMP3_CORRUPT_CONTROL=<binario viejo> bash tests/corruption.sh
```

La comparación `MS` contra `MK` se hace comprimiendo el mismo archivo con y sin
`-k4` y cortando los mismos bytes; los tres archivos que exhiben la clase A
están nombrados en la tabla de §2.
