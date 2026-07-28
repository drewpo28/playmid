/* ============================ FORMAT1 ENGINE BEGIN ============================
   Motor de reproducción: mezcla N pistas "al vuelo" (formato 1). El formato 0
   es el caso trivial de una sola pista y se reproduce con el mismo motor.
   Cada pista mantiene su propio offset dentro del fichero, su running status,
   el tick absoluto de su siguiente evento, y una caché de lectura propia.
   El bucle principal elige siempre la pista cuyo siguiente evento es el más
   cercano en el tiempo (merge por tick mínimo), espera hasta ese tick, y
   procesa el evento recargando la caché con F_SEEK + F_READ si hace falta.
   Los eventos de tempo (FF 51) se aplican globalmente, lo que con el merge
   por ticks da el resultado correcto sin más esfuerzo. */

// (All engine state — TRKST/tst/cur, tracks/curtrk/trkn/fired/wire_status,
// trk_next/l2_eof/trk_status/trk_end and the scalar working set — is declared
// next to the buffer, outside the 0x2000-0x2FFF budget: see the declarations
// above the engine section. Only tst itself still lives in the DATA segment.)

// OJO: el contador FRAMES (23672) NO sirve de reloj: mientras se ejecuta un comando
// punto, el manejador de 0x38 de la EPROM de esxdos no encadena con la ISR de la ROM
// y las variables del sistema quedan congeladas. Por eso contamos las interrupciones
// nosotros mismos con la ISR de IM2 (y el teclado se lee de los puertos, no de LASTK).

// Selecciona la pista activa: guarda la ventana de lectura y el estado de la anterior
// y carga los de la nueva en las globales espejo
void set_curtrk (BYTE t)
{
    if (t == curtrk)
        return;
    if (curtrk != 0xFF)
    {
        cur.cpos = rdptr - cptr;
        cur.clen = rdend - cptr;
        *curstate = cur;
    }
    curtrk = t;
    curstate = tst + t;
    cur = *curstate;
    cptr = tcaches + (WORD)t * tcsize;
    rdptr = cptr + cur.cpos;
    rdend = cptr + cur.clen;
}

// Valid bytes waiting in a track's L2 ring (< 64K: low words suffice). Factored
// out of l2_fillstep/trk_refill/l2_prefetch: each inlined its own copy of this
// subtraction, and SDCC (no cross-statement CSE) reloads every operand address
// from scratch each time, so the three copies cost noticeably more than one
// shared routine plus three calls.
WORD trk_remaining (TRKST *p)
{
    return *(WORD *)&p->l2end - *(WORD *)&p->off;
}

// One refill step of the active track's L2 ring: reads at most L2STAGE_SIZE bytes
// from the SD and appends them to the ring. Bounded to a few ms, so the SD cost is
// spread across many frames instead of stalling the music with whole-window reloads
// (frames that esxdos spends with our IM2 clock dismounted are lost, and a whole
// window is several frames in a row: it was heard as a glitch). The seek (a
// FAT-chain walk, the expensive part) is skipped when the last SD read was for this
// same track (sd_trk): reads of one track are sequential. l2_area is a power of
// two, so all ring arithmetic is masking with lmask — no multiplies, no carries.
// Scratch lives in globals: much cheaper than ix-indexed locals on the Z80.
void l2_fillstep (void)
{
    WORD off, chunk;

    xn = 0;            // callers poll xn to see whether the step landed
    if (l2_eof[curtrk])
        return;
    remt = trk_remaining (&cur);
    if (remt > (WORD)(lmask - (SDSTEP - 1)))
        return;                                       // no room for a whole step
    // Write position: the fill pointer only ever advances in steps of SDSTEP, so
    // it stays aligned and a step never straddles the window end or a 16KB bank
    // boundary. Crossing into the next window shows up in the bits above lmask;
    // the 16-bit wrap of the last window folds correctly through the subtract.
    fillb = cur.bank + remt;
    if ((fillb ^ cur.bank) & ~lmask)
        fillb -= l2_area;
    sd_enter ();       // esxdos runs with the bank-resident IM2 clock ticking
    if (sd_trk != curtrk)
    {
        seeknext ();   // to cur.l2end: a cheap forward hop from sdpos when possible
        sd_trk = curtrk;
    }
    // One call reads a whole 512-byte step into the bank-6 staging (pinned by
    // sd_enter anyway): the per-call cost dominates on image-backed media, so
    // bigger, rarer calls quarter both the tax and the stall count.
    xn = read (fhandle, (BYTE *)0 + SDSTAGE, SDSTEP);
    sd_exit ();        // esxdos done: merge the interrupts it sat on into int_cnt.
                       // The bounce copies below run on the normal clock (they
                       // are interrupt-safe)
    if (xn == 0xFFFF)
        xn = 0;
    if (xn)
    {
        // bounce staging -> target ring through L2STAGE, 128 bytes at a time
        for (off = 0; off < xn; off += chunk)
        {
            chunk = xn - off;
            if (chunk > 128)
                chunk = 128;
            bankmove (SDSTAGE + off, L2STAGE, chunk, 0);
            bankmove (fillb + off, L2STAGE, chunk, 1);
        }
        cur.l2end += xn;
        sdpos = cur.l2end;
    }
    // A full step consumes one unit of the track's chunk budget; at zero the whole
    // chunk sits in the ring (the last step may overshoot its end by <512 bytes:
    // harmless, FF 2F stops consumption before them). A short read is the file's
    // EOF. Either way no more SD time is ever spent on this track.
    if (xn < SDSTEP || !--trk_steps[curtrk])
        l2_eof[curtrk] = 1;
}

// Refills the active track's L1 cache from its L2 ring (a RAM copy). If the ring
// is dry (prefetching could not keep up), a single bounded step is read from the
// SD and we move on. No data at all -> end of track.
void trk_refill (void)
{
    xn = trk_remaining (&cur);
    if (xn == 0)
        l2_fillstep ();    // ring dry: borrow one bounded step; xn = bytes it read
    if (xn > tcsize)
        xn = tcsize;
    rem = l2_area - (cur.bank & lmask);               // contiguous run up to the window end
    if (xn > rem)
        xn = rem;
    if (xn == 0)
        trk_end[curtrk] = 1;
    else
    {
        bankmove (cur.bank, cptr, xn, 0);
        cur.bank += xn;
        if (!(cur.bank & lmask))                      // hit the window end: wrap to its base
            cur.bank -= l2_area;
        cur.off += xn;
    }
    rdptr = cptr;
    rdend = cptr + xn;
}

// Lee y consume el siguiente byte de la pista activa
BYTE trk_get (void)
{
    if (rdptr == rdend)
    {
        trk_refill();
        if (trk_end[curtrk])
            return 0;
    }
    return *rdptr++;
}

// Lee una cantidad de longitud variable (delta o longitud de metaevento/sysex).
// El caso comun (un solo byte) no hace ningun desplazamiento de 32 bits.
DWORD trk_varlen (void)
{
    DWORD v;

    c = trk_get();
    v = c & 0x7F;
    while (c & 0x80)
    {
        c = trk_get();
        v = (v<<7) | (c & 0x7F);
    }
    return v;
}

// Procesa un evento de la pista activa (el delta ya se consumió antes).
// A diferencia del reproductor de formato 0, aqui SIEMPRE se envia el byte de
// estado: el running status de la linea MIDI se rompe al intercalar pistas.
void trk_event (void)
{
    BYTE st, n, b;

    b = trk_get();
    if (b & 0x80)          // byte de estado nuevo
    {
        st = b;
        if (st < 0xF0)     // solo los estados de canal actualizan el running status:
            trk_status[curtrk] = st;   // sysex y metaeventos no lo cancelan (SMF es asi de laxo)
        b = 0xFF;          // señal: el primer byte de datos aun no se ha leido
    }
    else
        st = trk_status[curtrk];   // running status: b ya es el primer byte de datos

    // EVENTOS F0 y F7 (SYSEX). Se envian por trozos usando el buffer como staging.
    // Un sysex cancela el running status del cable.
    if (st == 0xF0 || st == 0xF7)
    {
        wire_status = 0;
        lbytes = trk_varlen();
        if (st == 0xF0)
        {
            buffer[0] = 0xF0;
            SendMIDI (buffer, 1);
        }
        while (lbytes)
        {
            n = (lbytes > 48) ? 48 : lbytes;   // el staging son los primeros 48 bytes del buffer
            for (i=0;i<n;i++)
                buffer[i] = trk_get();
            SendMIDI (buffer, n);
            lbytes -= n;
        }
        return;
    }

    // METAEVENTOS FF. Todos tienen la forma FF tipo longitud datos, asi que se
    // pueden saltar de forma generica. Solo tempo y fin de pista nos interesan.
    if (st == 0xFF)
    {
        param1 = trk_get();     // aqui b siempre es 0xFF: FF nunca llega por running status
        lbytes = trk_varlen();
        if (param1 == 0x2F)          // fin de pista
        {
            trk_end[curtrk] = 1;
            return;
        }
        if (param1 == 0x51 && lbytes == 3)   // Set Tempo: se aplica globalmente
        {
            ((BYTE *)&us_per_quarter)[3] = 0;
            ((BYTE *)&us_per_quarter)[2] = trk_get();
            ((BYTE *)&us_per_quarter)[1] = trk_get();
            ((BYTE *)&us_per_quarter)[0] = trk_get();
            settempo ();
            return;
        }
        while (lbytes--)             // el resto de metaeventos se ignora
            trk_get();
        return;
    }

    // EVENTOS de canal: estado + 1 o 2 bytes de datos, via staging. Si el estado
    // coincide con el último enviado por el cable, se omite (running status de salida):
    // a 31250 baudios cada byte ahorrado son 320us, y en los pasajes densos se nota.
    n = 0;
    if (st != wire_status)
    {
        wire_status = st;
        buffer[n++] = st;
    }
    buffer[n++] = (b != 0xFF) ? b : trk_get();
    if ((st & 0xE0) != 0xC0)       // C0 (program) y D0 (pressure) llevan 1 solo dato
        buffer[n++] = trk_get();
    SendMIDI (buffer, n);
}

// One prefetch step for the most depleted L2 ring, so the SD cost lands in the
// quiet gaps of the music instead of on top of dense passages. Remaining-byte
// counts fit in 16 bits.
//
// Always services the SINGLE worst live ring, not a round-robin candidate: the
// previous scheme kept topping up the track of the last SD read until its ring
// was completely full before considering anyone else, and even then it only
// advanced one rotating candidate per idle frame. A different, busier track
// could keep draining through many fired frames (l2_prefetch never runs then)
// while its turn in the rotation was still frames away — so its ring reached
// zero mid-passage and trk_refill()'s synchronous fallback had to run
// l2_fillstep() (im2_off + F_SEEK/F_READ) right in the middle of live events,
// stalling the IM2 clock while a note was already sounding: heard as a note
// hanging and dragging. Picking the worst ring outright bounds the response
// time to one idle frame regardless of track count, and still costs no extra
// seeks in the common case: a single 128-byte step barely moves the ring, so
// the same track is normally still the worst on the next idle frame too,
// keeping reads sequential (l2_fillstep already skips the seek when
// sd_trk == curtrk).
void l2_prefetch (void)
{
    c = 0xFF;               // c: worst track found so far (0xFF = none live)
    xn = 0xFFFF;            // xn: its remaining ring bytes
    rem = 0xFFFF;           // rem: sd_trk's remaining bytes (0xFFFF = not live)
    pst = tst;              // walk tst with a pointer: no per-track multiply
    for (trkn = 0; trkn < tracks; trkn++, pst++)
    {
        if (trk_end[trkn] || l2_eof[trkn])
            continue;
        // curtrk's tst slot is stale (its live state sits in cur between
        // switches): read the mirror instead so its real urgency is seen.
        remt = (trkn == curtrk) ? trk_remaining (&cur) : trk_remaining (pst);
        if (trkn == sd_trk)
            rem = remt;     // remember the last-read track's level as we pass it
        if (remt < xn)
        {
            xn = remt;
            c = trkn;
        }
    }
    if (c == 0xFF)                            // nobody live
        return;
    // Stickiness: while the last-read track's ring has room for a whole step,
    // keep serving IT (up to a FULL ring) even if another ring is lower — its
    // reads are sequential (no FAT-walk seek). Serving the strict minimum used
    // to ping-pong between two draining tracks, paying a seek nearly every step
    // (both hover just under the half watermark); long bursts amortize one seek
    // over ~16 steps.
    if (rem < l2_area - 127)
    {
        c = sd_trk;
        xn = rem;
    }
    else if (xn >= (l2_area >> 1))            // all above the low watermark
        return;
    // An event is due within ~2 frames: blocking on the SD now would audibly
    // delay it, so defer to a quieter frame — unless the chosen ring is nearly
    // dry (below a quarter), where a scheduled stall now beats an unscheduled
    // one at zero.
    if (hzbusy && xn >= (l2_area >> 2))
        return;
    set_curtrk (c);
    if (sd_trk != curtrk)
    {
        // Opening a new burst? The FAT-walk seek gets a frame of its own:
        // seek+read back to back is the longest blocking window there is, and
        // split apart each half delays the music half as much.
        sd_enter ();
        seeknext ();       // to cur.l2end
        sdpos = cur.l2end; // the pointer moved with no read to account for it:
                           // track it, or the next relative seek hops from a
                           // stale base
        sd_trk = curtrk;
        sd_exit ();
        sd_account ();     // recover any tick the call sat on (DI-holding kernels)
        return;
    }
    l2_fillstep ();        // sequential read, no seek: the shortest window
    if (xn)                // no SD call actually happened: nothing to account
        sd_account ();
}

// Bucle principal de reproduccion.
// ntrk es el numero de pistas que declara la cabecera MThd.
// Devuelve 0 si se ha reproducido, 1 si no se encontro ninguna pista MTrk.
// Recorre los chunks del fichero construyendo la tabla de offsets de comienzo de
// cada pista. La longitud de cada chunk está en su cabecera.
void scan_tracks (BYTE ntrk)
{
    DWORD len, fpos;

    tracks = 0;
    fpos = 14;
    pst = tst;
    lbytes = 0;        // running ring base (lbytes is free until playback starts)
    // (sdpos is poisoned high, so every seeknext here goes absolute; the scan does
    // not bother tracking the position — its seeks are shallow startup work)
    while (tracks < ntrk && tracks < MAX_TRACKS)
    {
        cur.l2end = fpos;      // seeknext target (cur is scratch until playback starts)
        seeknext ();
        if (read (fhandle, buffer, 8) != 8)
            break;
        // longitud del chunk (big-endian en el fichero), compuesta byte a byte:
        // mucho mas compacto que desplazamientos de 32 bits (somos little-endian)
        ((BYTE *)&len)[0] = buffer[7];
        ((BYTE *)&len)[1] = buffer[6];
        ((BYTE *)&len)[2] = buffer[5];
        ((BYTE *)&len)[3] = buffer[4];
        fpos += 8;
        if (((WORD *)buffer)[0] == 0x544D && ((WORD *)buffer)[1] == 0x6B72)   // "MTrk"
        {
            pst->off = fpos;
            pst->l2end = fpos;             // ring vacio: el primer uso lo rellena
            pst->bank = lbytes;            // each ring starts empty at its base
            pst->cpos = 0;
            pst->clen = 0;
            pst++;
            lbytes += l2_area;
            trk_steps[tracks] = (WORD)(len >> 9) + 1;   // +1: the last (partial) step
            l2_eof[tracks] = 0;
            trk_status[tracks] = 0;
            trk_end[tracks] = 0;
            tracks++;
        }
        fpos += len;    // chunks desconocidos se saltan sin contarlos
    }
}

BYTE playmidi1 (BYTE ntrk)
{
    BYTE best;

    // L2 ring per track: the largest power of two such that all the tracks fit in
    // the 64KB of banks (a power of two makes every ring wrap a masking operation).
    // Sized from the header's track count, so scan_tracks can hand out ring bases.
    // (The "i &&" guard stops the loop when i wraps to 0 on absurd track counts.)
    l2_area = 0x8000;                      // 1-2 tracks: two 32KB halves
    for (i = 2; i && i < ntrk; i <<= 1)
        l2_area >>= 1;
    // The bank-side IM2 clock lives in the top ~900 bytes of bank 6: when the
    // rings would fill all 64KB (track count a power of two), halve the ring
    // size so the allocator never reaches it.
    if ((DWORD)l2_area * ntrk > 0xFFFF)
        l2_area >>= 1;
    sd_im2_init ();                        // build the bank-side vector table/ISR
    lmask = l2_area - 1;
    sd_trk = 0xFF;

    scan_tracks (ntrk);
    // sd_account working set. Written only now: in the Z80 build est_read /
    // est_seek live where the init overlay code (scan_tracks) sat until the
    // call just above returned.
    p10 = us_per_int / 10;         // the frame length in 10us spin units (the
                                   // retroactive SD-call accounting, Z80 build)
    if (tracks == 0)
        return 1;       // el que llama imprime el error

    // Cuantas menos pistas, mas caché por pista (tope: 255, cpos/clen son BYTE).
    // Division by repeated subtraction: this runs once at startup (never in the
    // per-frame hot path), and it is the only WORD/WORD division in the whole
    // engine, so writing it out this way keeps SDCC's 16-bit division library
    // routine (__divuint and the smaller routines it chains to) out of the
    // binary entirely — real code-size budget, spent nowhere that matters.
    tcsize = 0;
    remt = TCACHE_TOTAL;
    while (remt >= tracks)
    {
        remt -= tracks;
        tcsize++;
    }
    if (tcsize > 255)
        tcsize = 255;

    // Leemos el primer delta de cada pista para inicializar su next_tick.
    // Una pista terminada se marca con next_tick = 0xFFFFFFFF (centinela): asi el
    // planificador no necesita consultar trk_end. De paso se llena del todo el ring
    // de cada pista: la espera cae aqui, cuando aun no suena nada (un seek por
    // pista y lecturas secuenciales), y con los ficheros que caben en los bancos
    // la SD no se vuelve a tocar durante la musica.
    curtrk = 0xFF;
    best = 0;                                  // contador de pistas vivas
    for (trkn = 0; trkn < tracks; trkn++)
    {
        set_curtrk (trkn);
        do
            l2_fillstep ();
        while (xn);                            // hasta ring lleno o EOF
        trk_next[trkn] = trk_varlen() << 6;    // <<6 == * PRECISION
        if (trk_end[trkn])
            trk_next[trkn] = 0xFFFFFFFF;
        else
            best++;
    }
    if (best == 0)
        return 0;

    // Planificador al estilo de zx-midiplayer: una pasada por TODAS las pistas en
    // cada frame ("¿te toca ya?": una sola comparacion por pista), y la pista que
    // esta al dia vacia de golpe todos sus eventos pendientes hasta la siguiente
    // pausa. Nada de buscar el tick minimo ni de reordenar: en los acordes densos
    // el coste por evento se queda en el parseo y el cable, no en el planificador.
    now = 0;
    tfrac = 0;
    wire_status = 0;
    im2_on ();         // desde aqui el reloj lo lleva la ISR
    cnt_last = int_cnt;
    while (1)
    {
        // Si pulsamos SPACE, salir
        if ((SEMIFILA8 & 0x1) == 0)
            return 0;

        // Un frame por vuelta: si la ISR no ha contado ninguno pendiente, dormimos.
        // Si vamos con retraso (un pasaje denso tardo mas de un frame), se procesan
        // vueltas seguidas sin dormir hasta ponerse al dia.
        hltf = 0;                  // assume catch-up pass (frame phase unknown)
        if (cnt_last == int_cnt)
        {
            WAIT_VRETRACE;
            hltf = 1;              // woke ON the interrupt: phase known from here
            txlast = 0;            // wire bytes sent this frame: none yet
            tx_flush ();   // just woke ON the frame interrupt: the whole queue goes
                           // out with the next /INT a full frame away (Z80 build);
                           // this halt and queue overflow (which self-credits) are
                           // the only two places that ever touch the wire
        }
        cnt_last++;
        now += ticks_per_int;
        tfrac += tpi_frac;         // accumulate the fractional ticks-per-frame...
        if (tfrac < tpi_frac)      // ...8-bit wrap = a whole PRECISION unit: carry it
            now++;

        // hznow = now + 2 frames of ticks: the sweep marks hzbusy when any
        // track's next event falls inside — an SD step taken then would audibly
        // delay those notes on slow media (see l2_prefetch)
        hznow = now + (ticks_per_int << 1);
        hzbusy = 0;
        fired = 0;
        pnext = trk_next;
        for (trkn = 0; trkn < tracks; trkn++, pnext++)
        {
            if (*pnext > now)          // aun no le toca (el centinela de pista
            {                          // terminada, 0xFFFFFFFF, nunca "toca")
                if (*pnext <= hznow)
                    hzbusy = 1;        // not due yet, but due inside the horizon
                continue;
            }
            fired = 1;
            set_curtrk (trkn);
            do
            {
                trk_event ();
                if (trk_end[trkn])
                {
                    *pnext = 0xFFFFFFFF;
                    if (--best == 0)   // no quedan pistas vivas
                        return 0;
                    break;
                }
                *pnext += trk_varlen() << 6;    // <<6 == * PRECISION
            }
            while (*pnext <= now);
            if (*pnext <= hznow)
                hzbusy = 1;            // its follow-up is close too
        }

        // Event-less frame: the perfect slot for one prefetch step — but only
        // on a halt-started pass: sd_account can then measure the SD call and
        // recover any interrupt it sat on (even under a DI-holding kernel); a
        // step on a catch-up pass would be unaccountable (Z80 build concern)
        if (!fired && hltf)
            l2_prefetch ();
    }
}

/* ============================ FORMAT1 ENGINE END ============================ */
