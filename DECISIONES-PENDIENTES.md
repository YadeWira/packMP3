# packMP3 — decisiones pendientes

Estado al 2026-08-23. Todo lo que sigue está medido; nada está aplicado sin
decisión. `master` está limpio, con **32 commits sobre el tag `v3.0f`**, y
`make test` pasa las dos suites.

Este archivo existe para que las decisiones no vivan solo en el historial de
una conversación. Cada punto trae el número que lo sostiene y lo que cuesta.

---

## 1. Cortar la v3.0g — **decisión pendiente**

Los 32 commits cierran **tres defectos de memoria que están vivos en la
versión publicada**. Los tres se miden, los tres tienen repro determinista, y
ninguno necesita nada exótico para alcanzarse.

| defecto | cómo se alcanza | commit |
|---|---|---|
| escritura fuera de rango en `pmp_predict_lame_anc` (1012 bytes más allá de un buffer de 2048) | **comprimiendo** un `.mp3` no confiable, o descomprimiendo | `43f12c7` |
| escritura fuera de rango en el `memset` de coeficientes (1102 bytes empezando 524 **antes** del buffer) | **un solo bit** dado vuelta en un `.pm3` válido | `71d8e2c` |
| lectura fuera de rango en `bandwidth_bounds[23]` | un solo bit; la versión publicada lo reporta como `bad huffman table` y sale sin pista | `1f3929b` |

El primero es el que más pesa: el sitio del **encoder** se alcanza al
*comprimir*, así que basta con que alguien pase un mp3 que no controla.

Además, sobre el régimen de daño chico —el que ninguno de mis barridos previos
tocaba— la mejora es grande y está medida sobre 12 archivos, 108 celdas:

|  | v3.0e (publicado) | HEAD |
|---|---|---|
| salidas silenciosamente incorrectas | 99 | **10** |
| crashes (grilla completa de 496 celdas) | 49 | **0** |

**Costo de cortarla**: el trabajo de release habitual. No rompe
compatibilidad — los `.pm3` siguen siendo los mismos bytes y las versiones
viejas los leen igual.

**Costo de no cortarla**: los tres defectos siguen vivos en lo que la gente
descarga.

---

## 2. Checksum o largo declarado en el formato — **decisión pendiente, más grande**

Quedan **14 celdas de 496** que decodifican a un MP3 incorrecto con exit 0 y
sin ninguna señal. No es que las guardas fallen: son archivos truncados que
resultan ser una **codificación válida de otro mp3**. Salida del tamaño
exacto del original, bytes distintos. No hay nada inconsistente que ningún
chequeo interno pueda encontrar.

Cerrarlo requiere que el contenedor declare un largo de payload o un checksum.

**Costo**: es un cambio de formato. Rompe compatibilidad hacia atrás, necesita
un `appversion` nuevo, y los archivos viejos habría que seguir leyéndolos por
el camino actual.

**Referencia**: packPNG lo resolvió por construcción en TCIJ v2 y reporta
300/300. packJPG y packMP3 tienen el mismo agujero y la misma respuesta
posible. Los tres proyectos están esperando la misma decisión.

La suite **pinea el 14** para que no crezca en silencio: si sube, falla.

---

## 3. Puntos menores — verificados hoy, ninguno urgente

- **Código de salida 0 cuando un archivo falla.** Un `.pm3` truncado se
  rechaza correctamente, con mensaje, y el proceso sale con 0. Rompe cualquier
  script que consulte el código de salida. Es anterior a todo este trabajo
  (verificado también contra v3.0c). Arreglarlo es un cambio de comportamiento
  visible para quien ya haya escrito scripts alrededor.
- **Dos mp3 corruptos que comprimen y después no se pueden descomprimir.**
  Medido: 2 de 150 entradas dañadas. El compresor acepta el archivo y el
  decompresor rechaza su propia salida (`sv_bound -302`, `coefficients`), o
  sea que el encoder emite valores que el decoder considera inválidos. Rompe
  el contrato sin pérdida, pero **no es silencioso** — el usuario recibe un
  error. Es anterior a todo este trabajo: en v3.0e los dos casos **crashean**,
  y con las guardas actuales son rechazos limpios. La suite lo pinea aparte
  del conteo de salidas incorrectas, en 2, para que no crezca sin que se note.
- **`-ver` verifica consistencia, no integridad.** Descomprime, recomprime y
  compara. Un archivo truncado que sea codificación válida de otro mp3 pasa
  `-ver` sin objeción, porque no hay nada inconsistente. Vale documentarlo en
  el README para que nadie lo lea como una garantía de integridad.

Dos puntos que estaban en esta lista ya no lo están, verificados hoy: el
archivo de salida de 0 bytes al fallar y el `Function description missing!`
en `list`/`stats` no se reproducen.

---

## 4. Fuera de alcance por decisión previa

- **Recompresión de carátulas GIF y WebP.** Descartado explícitamente
  («eso lo veremos en un futuro lejano»). No hay trabajo pendiente ni código a
  medio hacer.

---

## Qué hay en el árbol que no había ayer

Para que la decisión sobre la 3.0g se tome sabiendo qué se está publicando:

- `tests/corruption.sh` — seis regímenes de daño **declarados en el arnés**,
  no elegidos por corrida: bytes del final, porcentaje, un bit, tres bits, el **lado de
  entrada** (mp3 corrupto → comprimir → round-trip), y cada guarda con su caso
  determinista. 13 guardas pineadas por nombre.
- Un **canario por fuente**: el archivo sin dañar, por el mismo clasificador,
  obligado a volver idéntico. Sin él, un arnés roto imprime cero crashes, cero
  cuelgues y cero salidas incorrectas — el mejor resultado posible sobre una
  corrida que no midió nada.
- Eje de **duración de dos extremos**, porque el veredicto solo no distingue un
  decode sano de uno que tarda treinta segundos ni de uno que devuelve al
  instante sin trabajar.
- `make corrupt-asan` — los mismos regímenes bajo ASAN+UBSan. El primer uso
  encontró el tercer defecto de la tabla de arriba.
- Cada control corrido **en las dos direcciones**, y cada sondeo interno
  verificado por causa y no por haber producido salida.
