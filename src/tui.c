#include "tui.h"
#include "transient.h"
#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mpi.h>
#include <time.h>
#include <ida/ida.h>

/* ── global state ── */
static TUIProject g_proj;
static int g_running = 0;
static int g_paused  = 0;
static int g_quit    = 0;
static double g_speed = 1.0;
static double g_jump_to = -1;

/* ── forward decls ── */
static void screen_menu(void);
static void screen_builder(void);
static void screen_events(void);
static void screen_inspector(void);
static void screen_runner(void);
static void screen_results(void);
static void draw_boxed(int y, int x, int h, int w, const char *title);
static void center_text(int y, int w, const char *text);

/* ── helpers ── */
static const char *btype(BusType t) {
    switch(t) { case BUS_SLACK: return "SLACK"; case BUS_PV: return "PV"; case BUS_PQ: return "PQ"; default: return "?"; }
}
static const char *ftype_name(int t) {
    switch(t) { case FAULT: return "3ph"; case FAULT_SLG: return "SLG"; case FAULT_LL: return "LL"; case FAULT_DLG: return "DLG"; case FAULT_CLEAR: return "clear"; default: return "?"; }
}

static void draw_boxed(int y, int x, int h, int w, const char *title) {
    for (int i = 0; i < w; i++) { mvaddch(y, x+i, ACS_HLINE); mvaddch(y+h-1, x+i, ACS_HLINE); }
    for (int i = 0; i < h; i++) { mvaddch(y+i, x, ACS_VLINE); mvaddch(y+i, x+w-1, ACS_VLINE); }
    mvaddch(y, x, ACS_ULCORNER); mvaddch(y, x+w-1, ACS_URCORNER);
    mvaddch(y+h-1, x, ACS_LLCORNER); mvaddch(y+h-1, x+w-1, ACS_LRCORNER);
    if (title) { attron(A_BOLD); mvprintw(y, x+2, " %s ", title); attroff(A_BOLD); }
}

static void center_text(int y, int w, const char *text) {
    mvprintw(y, (w - (int)strlen(text))/2, "%s", text);
}

/* ── menu ── */
static void screen_menu(void) {
    const char *items[] = {"New Project", "Load Project", "Edit Events", "Network Info", "Run Simulation", "Help", "Quit", NULL};
    const char *keys = "NLIERHQ";
    int sel = 0, n = 0; while (items[n]) n++;
    clear();
    draw_boxed(0, 0, n+4, 50, " TSS — Transient Stability Simulator ");
    for (;;) {
        for (int i = 0; i < n; i++) {
            if (i == sel) attron(A_REVERSE);
                mvprintw(3+i, 3, "  %c  %-40s", keys[i], items[i]);
            if (i == sel) attroff(A_REVERSE);
        }
        if (g_proj.loaded)
            mvprintw(n+4, 2, "Project: %s (%d buses, %d gens)",
                     g_proj.proj_name, g_proj.sys.nbus, g_proj.sys.nmachines);
        int c = getch();
        if (c == 'q' || c == 27) { g_quit = 1; return; }
        if (c == KEY_UP && sel > 0) sel--;
        if (c == KEY_DOWN && sel < n-1) sel--;
        if (c == '\n' || c == KEY_ENTER) {
            switch (sel) {
            case 0: screen_builder(); break;
            case 1: { /* load */ screen_builder(); break; }
            case 2: screen_events(); break;
            case 3: screen_inspector(); break;
            case 4: if (g_proj.loaded) screen_runner(); break;
            case 5: break;
            case 6: default: g_quit=1; return;
            }
            clear(); draw_boxed(0,0,n+4,50," TSS — Transient Stability Simulator ");
        }
    }
}

/* ── builder ── */
static void screen_builder(void) {
    int h = LINES-2, w = COLS-2;
    int bus_scroll = 0, br_scroll = 0, gen_scroll = 0;
    int tab = 0; /* 0=buses, 1=branches, 2=gens */
    System *s = &g_proj.sys;

    if (!g_proj.loaded) {
        memset(s, 0, sizeof(*s));
        s->base_mva = 100.0;
        g_proj.arena = arena_new(1<<20);
        strcpy(g_proj.proj_name, "untitled");
        g_proj.loaded = 1;
        s->bus = NULL; s->branch = NULL; s->gen = NULL; s->load = NULL;
        s->machine = NULL;
    }

    clear();
    draw_boxed(0,0,h,w, " Network Builder ");
    mvprintw(1,2,"Project: %s  [Tab]switch [a]dd [Enter]edit [Del]ete [S]ave [Esc]back",
             g_proj.proj_name);

    for (;;) {
        int cw = w/3;
        /* buses */ wattron(stdscr, tab==0 ? A_BOLD : 0);
        draw_boxed(2,1, h-3, cw, " Buses ");
        mvprintw(3,2,"ID Name      kV  Type  Vm     Pd(MW)");
        int bi = 0;
        for (int i = bus_scroll; i < s->nbus && i < bus_scroll + h-6; i++, bi++) {
            Bus *b = &s->bus[i];
            if (i == bus_scroll+bi) mvprintw(4+bi,2,"%2d %-8s %4.0f %-5s %5.3f %6.0f",
                     b->id, b->name, b->base_kv, btype(b->type), b->vm, b->pd*s->base_mva);
        }
        wattroff(stdscr, tab==0 ? A_BOLD : 0);

        /* branches */ wattron(stdscr, tab==1 ? A_BOLD : 0);
        draw_boxed(2, cw+1, h-3, cw, "Branches");
        mvprintw(3,cw+2,"F  T   R       X        B");
        for (int i = br_scroll; i < s->nbranch && i < br_scroll+h-6; i++) {
            Branch *b = &s->branch[i];
            mvprintw(4+i-br_scroll,cw+2,"%2d %2d %7.5f %7.5f %7.5f",
                     s->bus[b->from].id, s->bus[b->to].id, b->r, b->x, b->b);
        }
        wattroff(stdscr, tab==1 ? A_BOLD : 0);

        /* gens */ wattron(stdscr, tab==2 ? A_BOLD : 0);
        draw_boxed(2, 2*cw+1, h-3, cw-1, "Generators");
        mvprintw(3,2*cw+2,"Bus  Pg(MW)  H     D   X'd");
        for (int i = gen_scroll; i < s->nmachines && i < gen_scroll+h-6; i++) {
            Machine *m = &s->machine[i];
            Gen *g = &s->gen[m->gen_idx];
            mvprintw(4+i-gen_scroll,2*cw+2,"%3d %6.0f %5.2f %4.1f %5.2f",
                     s->bus[g->bus].id, g->pg*s->base_mva, m->h, m->d, m->xdp);
        }
        wattroff(stdscr, tab==2 ? A_BOLD : 0);

        int c = getch();
        if (c == 27) return;
        if (c == '\t') { tab = (tab+1)%3; }
        if (c == 's' || c == 'S') {
            /* save project */
            if (s->nbus > 0) {
                snprintf(g_proj.raw_path, 256, "%s.raw", g_proj.proj_name);
                snprintf(g_proj.dyr_path, 256, "%s.dyr", g_proj.proj_name);
                FILE *rf = fopen(g_proj.raw_path, "w");
                if (rf) {
                    fprintf(rf,"     0, %.2f / PSS/E by TSS\n%s\nTSS\n", s->base_mva, g_proj.proj_name);
                    for (int i=0;i<s->nbus;i++){
                        Bus *b=&s->bus[i];
                        fprintf(rf,"%6d,'%-12s',%8.1f,%d,1,1,1,%8.5f,%8.4f,%8.1f,%8.1f,%.2f,%.2f\n",
                                b->id,b->name,b->base_kv,b->type,b->vm,b->va*180/M_PI,
                                b->pd*s->base_mva,b->qd*s->base_mva, b->vmax, b->vmin);
                    }
                    fprintf(rf,"0 / END BUS, BEGIN LOAD\n");
                    for (int i=0;i<s->nload;i++) fprintf(rf,"%d,%d,%8.1f,%8.1f,0,0,0,0,0,0\n",
                            s->load[i].id,s->bus[s->load[i].bus].id,s->load[i].p*s->base_mva,s->load[i].q*s->base_mva);
                    fprintf(rf,"0 / END LOAD, BEGIN GEN\n");
                    for (int i=0;i<s->ngen;i++) fprintf(rf,"%d,%d,%8.1f,%8.1f,9999,-9999,%.5f,0,%.1f,0,0,0,1,1,100,350,0,1,1\n",
                            s->gen[i].id,s->bus[s->gen[i].bus].id,s->gen[i].pg*s->base_mva,s->gen[i].qg*s->base_mva,s->gen[i].vsched,s->gen[i].mbase);
                    fprintf(rf,"0 / END GEN, BEGIN BRANCH\n");
                    for (int i=0;i<s->nbranch;i++)
                        fprintf(rf,"%6d,%6d,'1 ',%8.5f,%8.5f,%8.5f,9900,9900,9900,0,0,0,0,1,1.0000\n",
                                s->bus[s->branch[i].from].id,s->bus[s->branch[i].to].id,s->branch[i].r,s->branch[i].x,s->branch[i].b);
                    fprintf(rf,"0 / END BRANCH\nQ\n"); fclose(rf);
                }
                FILE *df = fopen(g_proj.dyr_path, "w");
                for (int i=0;i<s->nmachines;i++)
                    fprintf(df,"%d,'GENCLS',1,%8.3f,%8.1f,%8.3f\n",
                            s->bus[s->gen[s->machine[i].gen_idx].bus].id, s->machine[i].h, s->machine[i].d, s->machine[i].xdp);
                if (df) fclose(df);
                mvprintw(h-2,2,"Saved to %s.raw / %s.dyr  [any key]", g_proj.proj_name, g_proj.proj_name);
                getch();
            }
        }
        if (c == 'a' || c == 'A') {
            /* quick add with defaults */
            if (tab == 0) { /* add bus */
                Bus b; memset(&b,0,sizeof(b));
                b.id = s->nbus+1; b.base_kv=69; b.type=BUS_PQ; b.vm=1.0; b.va=0;
                snprintf(b.name,sizeof(b.name),"Bus%d",b.id);
                b.vmax=1.05; b.vmin=0.95;
                arrpush(s->bus,b); s->nbus++;
            } else if (tab == 1) { /* add branch */
                Branch br; memset(&br,0,sizeof(br));
                br.from = s->nbus>1 ? s->nbus-2 : 0;
                br.to = s->nbus>1 ? s->nbus-1 : 0;
                br.r=0.01; br.x=0.1; br.b=0; br.tap=1.0; br.status=1;
                snprintf(br.ckt,sizeof(br.ckt),"1");
                arrpush(s->branch,br); s->nbranch++;
            } else if (tab == 2) { /* add gen */
                int bi = s->nbus>0 ? s->nbus-1 : 0;
                Gen g; memset(&g,0,sizeof(g));
                g.id=1; g.bus=bi; g.pg=0; g.qg=0; g.mbase=100; g.vsched=1.0;
                g.machine_idx = s->nmachines;
                arrpush(s->gen,g); s->ngen++;
                Machine m; memset(&m,0,sizeof(m));
                m.gen_idx = s->ngen-1; m.h=5; m.d=5; m.xdp=0.1;
                arrpush(s->machine,m); s->nmachines++;
            }
        }
        if (c == KEY_DC && tab==0 && s->nbus>0) { s->nbus--; }
    }
}

/* ── events ── */
static void screen_events(void) {
    if (!g_proj.loaded) return;
    int h = LINES-2, sel=0;
    Event *ev = g_proj.events;
    int n = g_proj.n_events;
    clear(); draw_boxed(0,0,h,COLS-2," Event Editor ");
    for (;;) {
        mvprintw(2,2,"#  Time   Type   Bus  R       X      [a]dd [Del]ete [S]ave [Esc]back");
        for (int i=0; i<n && i<h-6; i++) {
            if (i==sel) attron(A_REVERSE);
            mvprintw(4+i,2,"%d  %5.2f %-5s %3d  %7.4f %7.4f",
                     i, ev[i].time, ftype_name(ev[i].type), ev[i].bus, ev[i].fault_r, ev[i].fault_x);
            if (i==sel) attroff(A_REVERSE);
        }
        int c = getch();
        if (c==27) return;
        if (c==KEY_UP && sel>0) sel--;
        if (c==KEY_DOWN && sel<n-1) sel++;
        if (c=='a') {
            Event e; memset(&e,0,sizeof(e));
            e.time=1.0; e.type=FAULT; e.bus=1; e.fault_r=0.001; e.fault_x=0.001;
            arrpush(g_proj.events,e); n=g_proj.n_events=arrlen(g_proj.events);
        }
        if (c==KEY_DC && n>0) {
            for (int i=sel;i<n-1;i++) g_proj.events[i]=g_proj.events[i+1];
            n = --g_proj.n_events;
        }
        if (c=='s') {
            snprintf(g_proj.ini_path,256,"%s.ini",g_proj.proj_name);
            FILE *f=fopen(g_proj.ini_path,"w");
            if(f){ for(int i=0;i<n;i++) fprintf(f,"[event]\ntime=%.2f\ntype=%s\nbus=%d\nr=%.4f\nx=%.4f\n\n",
                    ev[i].time,ftype_name(ev[i].type),ev[i].bus,ev[i].fault_r,ev[i].fault_x); fclose(f); }
            mvprintw(h-2,2,"Saved %s [any key]", g_proj.ini_path); getch();
        }
    }
}

/* ── inspector ── */
static void screen_inspector(void) {
    if (!g_proj.loaded) return;
    System *s = &g_proj.sys;
    int scr=0, h=LINES-2;
    clear(); draw_boxed(0,0,h,COLS-2," Network Inspector ");
    for (;;) {
        if (scr==0) {
            mvprintw(2,2,"Buses:  ID Name         Type   kV     V(pu)   Pd(MW)  Qd(MVar)");
            for (int i=0;i<s->nbus&&i<h-6;i++)
                mvprintw(4+i,2,"%4d %-12s %-5s %5.0f %7.3f %7.1f %7.1f",
                         s->bus[i].id,s->bus[i].name,btype(s->bus[i].type),
                         s->bus[i].base_kv,s->bus[i].vm,s->bus[i].pd*s->base_mva,s->bus[i].qd*s->base_mva);
        } else {
            mvprintw(2,2,"Branches: From  To  R        X        B        Tap   Status");
            for (int i=0;i<s->nbranch&&i<h-6;i++)
                mvprintw(4+i,2,"%4d %4d %8.5f %8.5f %8.5f %6.3f %s",
                         s->bus[s->branch[i].from].id,s->bus[s->branch[i].to].id,
                         s->branch[i].r,s->branch[i].x,s->branch[i].b,s->branch[i].tap,
                         s->branch[i].status?"ON":"OFF");
        }
        mvprintw(h-2,2,"[Tab] buses/branches  [Esc] back");
        int c = getch();
        if (c==27) return;
        if (c=='\t') scr=!scr;
    }
}

/* ── runner ── */
static void screen_runner(void) {
    System *s = &g_proj.sys;
    double t_end=5.0, t_step=0.01;
    int h=LINES-2, w=COLS-2;

    /* quick setup */
    Arena a2 = arena_new(1<<20);
    if (ybus_build(s, &a2)) { mvprintw(2,2,"YBUS FAILED"); getch(); return; }
    if (powerflow_solve(s, &a2)) { mvprintw(2,2,"PF FAILED"); getch(); return; }
    if (machine_init(s)) { mvprintw(2,2,"MACH FAILED"); getch(); return; }
    for (int i=0;i<s->nbus;i++){ Bus*b=&s->bus[i]; double v2=b->vm*b->vm; b->gl=v2>1e-10?b->pd/v2:0; b->bl=v2>1e-10?-b->qd/v2:0; }

    DAE dae; memset(&dae,0,sizeof(dae)); dae_init(&dae,s,&a2);
    Integrator itg; memset(&itg,0,sizeof(itg));
    if (integrator_init(&itg,&dae,0)) { mvprintw(2,2,"INT FAILED"); getch(); return; }

    double t=0, t_next=t_step, Vr_prev=0, Vi_prev=0;
    int step=0, ndiff=dae.ndiff, neq=dae.neq, ev_idx=0;
    double *y = N_VGetArrayPointer(itg.nvec_y);
    struct timespec rt_start; clock_gettime(CLOCK_MONOTONIC, &rt_start);
    double rt_t=0;

    clear(); draw_boxed(0,0,h,w," Simulation ");
    int paused=0, speed_pct=100;

    while (t < t_end-1e-10 && !g_quit) {
        /* process keyboard */
        int c;
        while ((c = getch()) != ERR) {
            if (c=='q'||c==27) { g_quit=1; break; }
            if (c=='p') paused=!paused;
            if (c=='+') speed_pct = speed_pct>=500?500:speed_pct+25;
            if (c=='-') speed_pct = speed_pct<=25?25:speed_pct-25;
            if (c=='e' && paused) {
                Event ev; memset(&ev,0,sizeof(ev)); ev.time=t; ev.type=FAULT; ev.bus=1; ev.fault_r=0.001; ev.fault_x=0.001;
                arrpush(g_proj.events,ev); g_proj.n_events=arrlen(g_proj.events);
                paused=0;
            }
        }
        if (g_quit) break;
        if (paused) { napms(50); continue; }

        double tout = t_next; if (tout>t_end) tout=t_end;
        double tret;
        int ret = IDASolve(itg.ida_mem, tout, &tret, itg.nvec_y, itg.nvec_ydot, IDA_NORMAL);
        if (ret<0) break;
        t=tret;

        /* display */
        for (int i=0;i<s->nbus&&i<h-8;i++) {
            double Vr=y[ndiff+2*i], Vi=y[ndiff+2*i+1], Vm=sqrt(Vr*Vr+Vi*Vi);
            int bar = (int)(Vm*20); if (bar>20) bar=20;
            mvprintw(3+i,2,"%2d %-5s ", s->bus[i].id, btype(s->bus[i].type));
            for (int b=0;b<20;b++) mvaddch(3+i,12+b, b<bar?'#'|A_REVERSE:' ');
            mvprintw(3+i,34,"%6.3f", Vm);
        }
        int col=45;
        for (int m=0;m<s->nmachines&&m<h-8;m++) {
            int bid=s->bus[s->gen[s->machine[m].gen_idx].bus].id;
            double d=y[2*m]*180/M_PI;
            int bar=abs((int)d/2); if(bar>20)bar=20;
            mvprintw(3+m,col,"G@%d ",bid);
            for(int b=0;b<20;b++) mvaddch(3+m,col+5+b,(d>0?b<bar:b<bar)?(d>0?'#'|A_REVERSE:'-'|A_REVERSE):' ');
            mvprintw(3+m,col+27,"%+6.1f°",d);
        }
        double pct = (t/t_end)*100;
        mvprintw(h-5,2,"Progress: [");
        for(int b=0;b<30;b++) mvaddch(h-5,13+b,b<(int)(pct*30/100)?'=':' ');
        mvprintw(h-5,44,"] %5.1f%% t=%.2f",pct,t);
        mvprintw(h-3,2,"Speed:%3d%% [p]ause [+/-]speed [e]vent [q]uit", speed_pct);
        refresh();

        /* real-time sync */
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        double wall=(now.tv_sec-rt_start.tv_sec)+(now.tv_nsec-rt_start.tv_nsec)*1e-9;
        double lag=(t-rt_t)/ (speed_pct/100.0) - (wall - rt_t/(speed_pct/100.0));
        if (lag>0.001) napms((int)(lag*1000));
        rt_t=t; clock_gettime(CLOCK_MONOTONIC, &rt_start);

        step++; while(t_next<=t+1e-10) t_next+=t_step;
    }

    /* results */
    clear(); draw_boxed(0,0,h,w," Simulation Complete ");
    mvprintw(3,2,"t=%.3fs  steps=%d", t, step);
    mvprintw(5,2,"Bus voltage extremes:");
    for (int i=0;i<s->nbus&&i<h-10;i++) {
        double Vr=y[ndiff+2*i], Vi=y[ndiff+2*i+1];
        mvprintw(6+i,4,"Bus %2d %-5s V=%.4f", s->bus[i].id, btype(s->bus[i].type), sqrt(Vr*Vr+Vi*Vi));
    }
    mvprintw(h-2,2,"[any key] back");
    getch();

    integrator_free(&itg); dae_free(&dae);
}

/* ── results viewer stub ── */
static void screen_results(void) {
    clear(); mvprintw(2,2,"Results — not yet implemented. [any key]"); getch();
}

/* ── main ── */
int tui_main(void) {
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0);
    start_color();
    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);

    memset(&g_proj, 0, sizeof(g_proj));
    g_proj.arena = arena_new(1<<20);

    screen_menu();

    if (g_proj.sys.colptr) free(g_proj.sys.colptr);
    if (g_proj.sys.rowidx) free(g_proj.sys.rowidx);
    if (g_proj.sys.yval) free(g_proj.sys.yval);
    free(g_proj.sys.machine_states);
    arrfree(g_proj.events);
    arrfree(g_proj.sys.machine);
    arrfree(g_proj.sys.bus);
    arrfree(g_proj.sys.branch);
    arrfree(g_proj.sys.gen);
    arrfree(g_proj.sys.load);
    arena_free(&g_proj.arena);

    endwin();
    return 0;
}
