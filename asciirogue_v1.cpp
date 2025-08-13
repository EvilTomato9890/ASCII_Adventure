
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
  #include <conio.h>
  #include <windows.h>
#else
  #include <termios.h>
  #include <unistd.h>
#endif

namespace io {
#ifdef _WIN32
bool enableVT() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return false;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return false;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return SetConsoleMode(hOut, dwMode);
}
int getch_blocking(){ return _getch(); }
#else
struct TermiosGuard{ termios oldt{}; bool active=false;
    void enableRaw(){ if(tcgetattr(STDIN_FILENO,&oldt)==-1) return; termios t=oldt; t.c_lflag &= ~(ICANON|ECHO); t.c_cc[VMIN]=1; t.c_cc[VTIME]=0; if(tcsetattr(STDIN_FILENO,TCSANOW,&t)==-1) return; active=true; }
    ~TermiosGuard(){ if(active) tcsetattr(STDIN_FILENO,TCSANOW,&oldt); }
};
int getch_blocking(){ unsigned char c; if(read(STDIN_FILENO,&c,1)!=1) return -1; return (int)c; }
bool enableVT(){ return true; }
#endif
void clear(){ std::cout << "\x1b[2J\x1b[H"; }
void move(int r,int c){ std::cout << "\x1b["<<(r+1)<<";"<<(c+1)<<"H"; }
void hideCursor(){ std::cout << "\x1b[?25l"; }
void showCursor(){ std::cout << "\x1b[?25h"; }
void flush(){ std::cout.flush(); }
} // namespace io
struct Pos;
// Forward declarations
struct Pos;
struct Game;
static void ai_turn(Game& g);
static void process_statuses(Game& g);
static bool target_tile(Game& g,int range, Pos& out);
static void explode_at(Game& g, int r, int c, int radius);
static void level_up(Game& g);
static void world_tick(Game& g);


// Forward decls
struct Game;
static void explode_at(Game& g, int r, int c, int radius);


// ---------------- RNG ----------------
struct RNG{ std::mt19937_64 eng; RNG():eng(std::random_device{}()){} explicit RNG(uint64_t s):eng(s){} int i(int lo,int hi){ std::uniform_int_distribution<int>d(lo,hi);
 return d(eng);
} double d(double lo,double hi){ std::uniform_real_distribution<double>d(lo,hi);
 return d(eng);
} bool chance(double p){ std::bernoulli_distribution b(p);
 return b(eng);
} };

// ---------------- Basics ----------------
struct Pos{ int r=0,c=0; };
inline bool operator==(const Pos&a,const Pos&b){ return a.r==b.r && a.c==b.c; }
inline bool operator!=(const Pos&a,const Pos&b){ return !(a==b); }
struct Rect{ int r=0,c=0,h=0,w=0; };

// ---------------- Tiles ----------------
enum class Tile:int{ Wall=0, Floor=1, StairsDown=2, DoorClosed=3, DoorOpen=4, TrapHidden=5, TrapRevealed=6, SecretWall=7, Teleporter=8};
struct Cell{ Tile t=Tile::Wall; bool visible=false, seen=false; };
struct Map{
    int H=24,W=80; std::vector<Cell> g;
    Map(int h,int w):H(h),W(w),g(h*w){}
    Cell& at(int r,int c){ return g[r*W+c]; }
    const Cell& at(int r,int c) const { return g[r*W+c]; }
    bool in(int r,int c) const { return r>=0&&c>=0&&r<H&&c<W; }
    bool walkable(int r,int c) const { if(!in(r,c)) return false; Tile t=at(r,c).t; return !(t==Tile::Wall||t==Tile::DoorClosed||t==Tile::SecretWall); }
    void resetFOV(){ for(auto&x:g){ x.visible=false; } }
};

// ---------------- Entities/Items ----------------
enum class ItemKind{
    PotionHeal,PotionStr,PotionAntidote,PotionRegen,
    Dagger,Sword,
    ArmorLeather,ArmorChain,
    Key,
    SpellbookFirebolt,SpellbookHeal,SpellbookBlink,SpellbookIce,SpellbookShield,SpellbookFireball,
    ScrollBlink,ScrollMapping,
    Bomb
};
enum class AiKind{ Wander,Hunter };
enum class EntityType{ Player,Mob,ItemEntity,Chest,Merchant,BombPlaced };
enum class SpellKind{ Firebolt,Heal,Blink,IceShard,Shield,Fireball };

enum class TrapKind{ Spike,Fire,Snare,Poison,Teleport,Explosive };

struct Stats{
    int max_hp=20,hp=20;
    int atk=3,def=1,str=10;
    int max_mp=10,mp=10;
    // durations
    int burning=0,snared=0,poison=0,regen=0,shield=0;
};

struct Item{ ItemKind kind; std::string name; char glyph='?'; int power=0; };

struct Inventory{
    std::vector<Item> items; int weapon_idx=-1, armor_idx=-1; int keys=0; std::vector<SpellKind> spells; std::unordered_map<int,int> mastery;
    bool knows(SpellKind s) const { return std::find(spells.begin(),spells.end(),s)!=spells.end(); }
    int boost(SpellKind s) const { auto it=mastery.find((int)s); return it==mastery.end()?0:it->second; }
    void learn(SpellKind s){ if(!knows(s)) spells.push_back(s); mastery[(int)s]++; }
};

struct Monster{ std::string name="mob"; char glyph='m'; Stats st; AiKind ai=AiKind::Wander; bool alive=true; int xp=5; int speed=100; int energy=0; };

struct Chest{ bool locked=true; bool opened=false; Item content{}; };

struct Entity{
    EntityType type=EntityType::Mob; Pos pos; bool blocks=true;
    Monster mob; Item item; Chest chest; int fuse=0;
};

// ---------------- Game ----------------
struct Options{ bool auto_open_on_bump=true; bool auto_pickup_keys=true; };
struct Log{ std::vector<std::string> lines; void add(const std::string&s){ lines.push_back(s);
 if(lines.size()>400) lines.erase(lines.begin(),lines.begin()+200);
} void render(int H,int W){ int start=(int)std::max(0,(int)lines.size()-3);
 for(int i=0;i<3;i++){ int idx=start+i; io::move(H-3+i,0);
 std::string row=(idx<(int)lines.size()? lines[idx]:"");
 if((int)row.size()>W) row.resize(W);
 std::cout<< std::left << std::setw(W) << row; } } };

struct Game{
    Map map; RNG rng; int level=1,max_level=8; std::string biome="Default";
    Entity player; Inventory inv; std::vector<Entity> ents; Log log; bool running=true; int gold=0;
    Pos teleporter{ -1, -1 };
    
    std::vector<Pos> firezones;
    std::vector<int> firettl;
// camera
    int cam_r=0, cam_c=0; bool cam_follow=true;
    // meta
    int xp=0, plv=1; std::unordered_map<std::string,int> kills;
    Options opt;
    Game(int h=24,int w=80): map(h,w) {}
};

// ---------------- Helpers ----------------
static std::vector<Pos> neighbors4(int r,int c){ return {{r-1,c},{r+1,c},{r,c-1},{r,c+1}}; }
static bool opaque(const Map& m,int r,int c){ if(!m.in(r,c)) return true; Tile t=m.at(r,c).t; return t==Tile::Wall || t==Tile::DoorClosed || t==Tile::SecretWall; }
static char tile_glyph(const Cell& cell){
    switch(cell.t){
        case Tile::Wall: return '#';
        case Tile::Floor: return '.';
        case Tile::StairsDown: return '>';
        case Tile::DoorClosed: return '+';
        case Tile::DoorOpen: return '/';
        case Tile::TrapHidden: return '^';
        case Tile::TrapRevealed: return '^';
        case Tile::SecretWall: return 'x'; // cracked wall
        case Tile::Teleporter: return 'T';
    }
    return '?';
}

static void set_visible(Map&m,int r,int c){ if(m.in(r,c)){ m.at(r,c).visible=true; m.at(r,c).seen=true; } }
static bool los_block(const Map&m,int r,int c){ return opaque(m,r,c); }

static void bres_line(int x0,int y0,int x1,int y1, std::vector<Pos>& out){
    int dx=std::abs(x1-x0), sx=x0<x1?1:-1;
    int dy=-std::abs(y1-y0), sy=y0<y1?1:-1;
    int err=dx+dy, e2;
    while(true){
        out.push_back({x0,y0});
        if(x0==x1 && y0==y1) break;
        e2 = 2*err;
        if(e2 >= dy){ err += dy; x0 += sx; }
        if(e2 <= dx){ err += dx; y0 += sy; }
    }
}

static void compute_fov(Map&m,int cx,int cy,int radius){
    m.resetFOV();
    set_visible(m,cx,cy);
    for(int r=cx-radius; r<=cx+radius; ++r){
        for(int c=cy-radius; c<=cy+radius; ++c){
            if(!m.in(r,c)) continue;
            int dist = std::abs(r-cx)+std::abs(c-cy);
            if(dist>radius) continue;
            std::vector<Pos> line;
            bres_line(cx,cy,r,c,line);
            bool blocked=false;
            for(size_t i=1;i<line.size();++i){
                Pos p=line[i];
                if(!m.in(p.r,p.c)) break;
                set_visible(m,p.r,p.c);
                if(los_block(m,p.r,p.c)){
                    blocked=true;
                    break;
                }
            }
        }
    }
}
// ---------------- Pathfinding ----------------
struct PQE{ int f,g,r,c; };
static std::vector<Pos> astar(const Map&m,Pos s,Pos t){
    auto h=[&](int r,int c){ return std::abs(r-t.r)+std::abs(c-t.c); };
    auto cmp=[](const PQE&a,const PQE&b){ return a.f>b.f || (a.f==b.f && a.g<b.g); };
    std::vector<PQE> open; 
    std::unordered_map<long long,std::pair<int,int>> parent; std::unordered_set<long long> inOpen; std::unordered_map<long long,int> bestG;
    auto key=[&](int r,int c)->long long{ return ((long long)r<<32)^(unsigned)c; };
    auto push=[&](int r,int c,int g){ PQE e{g+h(r,c),g,r,c}; open.push_back(e);
 std::push_heap(open.begin(),open.end(),cmp);
 inOpen.insert(key(r,c));
 };
    push(s.r,s.c,0); bestG[key(s.r,s.c)]=0;
    while(!open.empty()){
        std::pop_heap(open.begin(),open.end(),cmp);
 PQE cur=open.back();
 open.pop_back();
 inOpen.erase(key(cur.r,cur.c));

        if(cur.r==t.r && cur.c==t.c){ std::vector<Pos> path; long long k=key(t.r,t.c);
 while(true){ path.push_back({(int)(k>>32),(int)(k&0xffffffff)});
 auto it=parent.find(k);
 if(it==parent.end()) break; k=((long long)it->second.first<<32)^(unsigned)it->second.second; } std::reverse(path.begin(),path.end());
 return path; }
        for(auto nb: neighbors4(cur.r,cur.c)){
            if(!m.walkable(nb.r,nb.c)) continue; int ng=cur.g+1; long long nk=key(nb.r,nb.c);
            auto it=bestG.find(nk);
 if(it==bestG.end()||ng<it->second){ bestG[nk]=ng; parent[nk]={cur.r,cur.c}; if(!inOpen.count(nk)) push(nb.r,nb.c,ng);
 }
        }
    }
    return {};
}

// ---------------- Content tables ----------------
static std::vector<std::string> monster_names={"rat","bat","kobold","goblin","orc","worm","snake","slime","skeleton","zombie","wolf","boar","imp","harpy","ghoul","shade","spider","centipede","beetle","fungus","cultist","bandit","brigand","thug","warlock","witch","acolyte","scout","archer","hound"};
static std::vector<std::string> weapon_names={"rusty dagger","bone dagger","steel dagger","short sword","serrated sword","long sword","elven blade","orcish cleaver","rapier","falchion","gladius"};
static std::vector<std::string> armor_names={"ragged tunic","leather jerkin","studded leather","chain shirt","scale mail","lamellar","brigandine","elven mail","orcish hauberk","dwarf mail"};

// ---------------- Gen helpers ----------------
static bool rect_overlap(const Rect&a,const Rect&b){ return !(a.r+a.h<=b.r || b.r+b.h<=a.r || a.c+a.w<=b.c || b.c+b.w<=a.c); }
static void carve_room(Map&m,const Rect&R){ for(int r=R.r;r<R.r+R.h;r++) for(int c=R.c;c<R.c+R.w;c++) if(m.in(r,c)) m.at(r,c).t=Tile::Floor; }
static void carve_h(Map&m,int r,int c1,int c2){ if(c2<c1) std::swap(c1,c2); for(int c=c1;c<=c2;c++) if(m.in(r,c)) m.at(r,c).t=Tile::Floor; }
static void carve_v(Map&m,int c,int r1,int r2){ if(r2<r1) std::swap(r1,r2); for(int r=r1;r<=r2;r++) if(m.in(r,c)) m.at(r,c).t=Tile::Floor; }
static Pos center(const Rect&R){ return {R.r+R.h/2, R.c+R.w/2}; }
static bool is_door_site(const Map&m,int r,int c){
    if(!m.in(r,c) || m.at(r,c).t!=Tile::Floor) return false;
    int wallN=(m.in(r-1,c)&&m.at(r-1,c).t==Tile::Wall);
    int wallS=(m.in(r+1,c)&&m.at(r+1,c).t==Tile::Wall);
    int wallW=(m.in(r,c-1)&&m.at(r,c-1).t==Tile::Wall);
    int wallE=(m.in(r,c+1)&&m.at(r,c+1).t==Tile::Wall);
    if(wallN&&wallS && !wallW && !wallE) return true;
    if(wallW&&wallE && !wallN && !wallS) return true;
    return false;
}

// ---------------- Items/Monsters ----------------
static Item make_random_item(RNG&rng){
    int t=rng.i(0, 17);
    Item it{};
    switch(t){
        case 0: it.kind=ItemKind::PotionHeal; it.name="potion of healing"; it.glyph='!'; it.power=rng.i(4,9); break;
        case 1: it.kind=ItemKind::PotionStr; it.name="potion of strength"; it.glyph='!'; it.power=rng.i(1,2); break;
        case 2: it.kind=ItemKind::PotionAntidote; it.name="potion of antidote"; it.glyph='!'; break;
        case 3: it.kind=ItemKind::PotionRegen; it.name="potion of regeneration"; it.glyph='!'; it.power=rng.i(4,6); break;
        case 4: it.kind=ItemKind::Dagger; it.name=weapon_names[rng.i(0,(int)weapon_names.size()-1)]; it.glyph=')'; it.power=rng.i(1,2); break;
        case 5: it.kind=ItemKind::Sword; it.name=weapon_names[rng.i(0,(int)weapon_names.size()-1)]; it.glyph=')'; it.power=rng.i(2,4); break;
        case 6: it.kind=ItemKind::ArmorLeather; it.name=armor_names[rng.i(0,(int)armor_names.size()-1)]; it.glyph='['; it.power=rng.i(1,2); break;
        case 7: it.kind=ItemKind::ArmorChain; it.name=armor_names[rng.i(0,(int)armor_names.size()-1)]; it.glyph='['; it.power=rng.i(2,4); break;
        case 8: it.kind=ItemKind::Key; it.name="small key"; it.glyph=';'; it.power=1; break;
        case 9: it.kind=ItemKind::SpellbookFirebolt; it.name="spellbook: Firebolt"; it.glyph='?'; break;
        case 10: it.kind=ItemKind::SpellbookHeal; it.name="spellbook: Heal"; it.glyph='?'; break;
        case 11: it.kind=ItemKind::SpellbookBlink; it.name="spellbook: Blink"; it.glyph='?'; break;
        case 12: it.kind=ItemKind::SpellbookIce; it.name="spellbook: Ice Shard"; it.glyph='?'; break;
        case 13: it.kind=ItemKind::ScrollMapping; it.name="scroll of mapping"; it.glyph='?'; break;
        case 14: it.kind=ItemKind::Bomb; it.name="bomb"; it.glyph='o'; it.power=1; break;
        case 15: it.kind=ItemKind::Key; it.name="small key"; it.glyph=';'; it.power=1; break;
        default: it.kind=ItemKind::SpellbookShield; it.name="spellbook: Shield"; it.glyph='?'; break;
    } return it;
}
static std::string item_desc(const Item& it){
    switch(it.kind){
        case ItemKind::PotionHeal: return "Heals HP";
        case ItemKind::PotionStr: return "Permanently +STR";
        case ItemKind::PotionAntidote: return "Cures poison";
        case ItemKind::PotionRegen: return "Regenerates HP over time";
        case ItemKind::Dagger: case ItemKind::Sword: return "Weapon +" + std::to_string(it.power);
        case ItemKind::ArmorLeather: case ItemKind::ArmorChain: return "Armor +" + std::to_string(it.power);
        case ItemKind::Key: return "Opens locked chests";
        case ItemKind::SpellbookFirebolt: return "Learn: Firebolt";
        case ItemKind::SpellbookHeal: return "Learn: Heal";
        case ItemKind::SpellbookBlink: return "Learn: Blink";
        case ItemKind::SpellbookIce: return "Learn: Ice Shard";
        case ItemKind::SpellbookShield: return "Learn: Shield";
        case ItemKind::SpellbookFireball: return "Learn: Fireball";
        case ItemKind::ScrollBlink: return "Teleport a few tiles";
        case ItemKind::ScrollMapping: return "Reveal the map";
        case ItemKind::Bomb: return "Create an explosion";
    } return "";
}
static Monster make_mon(RNG&rng,int level){
    Monster m{}; m.name=monster_names[rng.i(0,(int)monster_names.size()-1)];
    m.glyph="mnbhkowzsWISGHsiCFBbwWaA"[rng.i(0,(int)std::string("mnbhkowzsWISGHsiCFBbwWaA").size()-1)];
    m.st.max_hp=m.st.hp=rng.i(5+level, 10+level*2);
    m.st.atk=rng.i(1+level/2, 3+level);
 m.st.def=rng.i(0,2+level/2);
 m.st.str=rng.i(6,12+level);

    m.ai=rng.chance(0.6)?AiKind::Hunter:AiKind::Wander; m.xp=4+level*2; m.speed= rng.i(70,130); m.energy=0; return m;
}

// ---------------- Generation ----------------
static std::vector<Rect> generate_dungeon(Map& m,RNG& rng,std::string& biome){
    m.g.assign(m.H*m.W,Cell{});
    const char* biomes[]={"Crypt","Catacombs","Armory","Lava Caves","Sewers","Library"};
    biome=biomes[rng.i(0,5)];
    int rooms=rng.i(10,16); std::vector<Rect> R; int attempts=0;
    while((int)R.size()<rooms && attempts<350){
        attempts++; int h=rng.i(4,7), w=rng.i(5,11);
 int r=rng.i(1,m.H-h-2), c=rng.i(1,m.W-w-2);

        Rect t{r,c,h,w}; bool ok=true; for(auto&q:R) if(rect_overlap(t,q)){ok=false;break;} if(!ok) continue; R.push_back(t);
    }
    for(auto&q:R) carve_room(m,q);
    std::vector<Pos> centers; for(auto&q:R) centers.push_back(center(q));
    std::vector<int> order(centers.size());
 for(size_t i=0;i<order.size();
i++) order[i]=(int)i; std::shuffle(order.begin(),order.end(),rng.eng);

    for(size_t i=1;i<order.size();
i++){ Pos a=centers[order[i-1]], b=centers[order[i]]; if(rng.chance(0.5)){carve_h(m,a.r,a.c,b.c);
 carve_v(m,b.c,a.r,b.r);
} else {carve_v(m,a.c,a.r,b.r);
 carve_h(m,b.r,a.c,b.c);
} }
    if(!R.empty()){ Pos s=center(R.back()); m.at(s.r,s.c).t=Tile::Teleporter; }
    for(int r=1;r<m.H-1;r++) for(int c=1;c<m.W-1;c++){ if(is_door_site(m,r,c) && rng.chance(0.35)) m.at(r,c).t=Tile::DoorClosed; }
    double trap_rate=(biome=="Lava Caves"?0.08: biome=="Catacombs"?0.05: 0.04);
    for(int r=1;r<m.H-1;r++) for(int c=1;c<m.W-1;c++){ if(m.at(r,c).t==Tile::Floor && rng.chance(trap_rate)) m.at(r,c).t=Tile::TrapHidden; }
    return R;
}
static void place_player(Game& g,const std::vector<Rect>& rooms){ g.player.pos=rooms.empty()? Pos{1,1}: center(rooms.front()); }
static void place_mobs_items_chests(Game& g,const std::vector<Rect>& rooms){
    for(size_t i=1;i<rooms.size();i++){
        Pos p=center(rooms[i]);
        if(g.rng.chance(0.80)){ Entity e{}; e.type=EntityType::Mob; e.pos=p; e.mob=make_mon(g.rng,g.level);
 g.ents.push_back(e);
 }
        if(g.rng.chance(0.65)){ Entity it{}; it.type=EntityType::ItemEntity; it.blocks=false; it.pos={p.r+g.rng.i(-1,1), p.c+g.rng.i(-1,1)}; if(!g.map.in(it.pos.r,it.pos.c)||!g.map.walkable(it.pos.r,it.pos.c)) it.pos=p; it.item=make_random_item(g.rng);
 g.ents.push_back(it);
 }
        if(g.rng.chance(0.45)){ Entity ch{}; ch.type=EntityType::Chest; ch.blocks=false; ch.pos=p; ch.chest.locked=g.rng.chance(0.65);
 ch.chest.opened=false; ch.chest.content=make_random_item(g.rng);
 g.ents.push_back(ch);
 }
    }
}

// ---------------- Combat/Status ----------------
static int roll_damage(RNG&rng,int atk,int def){ int base=std::max(0,atk-def);
 int var=rng.i(0,2);
 return std::max(0,base+var);
 }
static void apply_status_tick(Game& g, Stats& st, bool is_player){
    if(st.burning>0){ st.hp-=1; st.burning--; if(is_player) g.log.add("You are burning!"); }
    if(st.poison>0){ st.hp-=1; st.poison--; if(is_player) g.log.add("You suffer poison."); }
    if(st.regen>0){ st.hp=std::min(st.max_hp, st.hp+1);
 st.regen--; if(is_player) g.log.add("You regenerate.");
 }
    if(st.snared>0){ st.snared--; }
    if(st.shield>0){ st.shield--; }
}
static void process_statuses(Game& g){
    apply_status_tick(g,g.player.mob.st,true);
    for(auto& e: g.ents) if(e.type==EntityType::Mob && e.mob.alive){ int before=e.mob.st.hp; apply_status_tick(g,e.mob.st,false);
 if(e.mob.st.hp<=0){ e.mob.alive=false; g.log.add(e.mob.name+" dies from ailments.");
 } }
}
static void grant_xp(Game& g,int amt){ g.xp += amt; g.log.add("You gain "+std::to_string(amt)+" XP."); level_up(g); }
static int xp_to_next(int plv){ return 10 + plv*10; }
static void level_up(Game& g){
    while(g.xp >= xp_to_next(g.plv)){
        g.xp -= xp_to_next(g.plv); g.plv++;
        g.player.mob.st.max_hp += 2; g.player.mob.st.hp = g.player.mob.st.max_hp;
        g.player.mob.st.atk += 1; g.player.mob.st.max_mp += 1; g.player.mob.st.mp = g.player.mob.st.max_mp;
        g.log.add("Level up! You are now level "+std::to_string(g.plv)+".");
        // 25% chance to learn a random spell
        if(g.rng.chance(0.25)){
            SpellKind s = (SpellKind)g.rng.i(0,4);
            if(!g.inv.knows(s)){ g.inv.learn(s);
 g.log.add("You intuit a new spell.");
 }
        }
    }
}

static void attack(Game& g, Entity& A, Entity& B, const std::string& aname, const std::string& bname){
    int atk = A.mob.st.atk;
    int def = B.mob.st.def;
    // player weapon bonus
    if(A.type==EntityType::Player && g.inv.weapon_idx>=0 && g.inv.weapon_idx<(int)g.inv.items.size()){
        const Item& w = g.inv.items[g.inv.weapon_idx];
        if(w.kind==ItemKind::Dagger) atk += 2;
        if(w.kind==ItemKind::Sword) atk += 4;
    }
    // armor reduces damage passively (already in def), but if player has armor equipped, increase def
    if(B.type==EntityType::Player && g.inv.armor_idx>=0 && g.inv.armor_idx<(int)g.inv.items.size()){
        const Item& ar = g.inv.items[g.inv.armor_idx];
        if(ar.kind==ItemKind::ArmorLeather) def += 1;
        if(ar.kind==ItemKind::ArmorChain) def += 2;
    }
    int dmg = std::max(1, atk - def + g.rng.i(0,2));
    B.mob.st.hp -= dmg;
    g.log.add(aname+" hit "+bname+" for "+std::to_string(dmg)+".");
    if(B.mob.st.hp<=0 && B.type==EntityType::Mob){
        B.mob.alive=false; g.log.add(bname+" dies."); grant_xp(g,B.mob.xp); g.kills[B.mob.name]++;
    }
}

// ---------------- Inventory ----------------
static void pickup(Game& g){
    for(size_t i=0;i<g.ents.size();++i){
        auto&e=g.ents[i];
        if(e.type==EntityType::ItemEntity && e.pos==g.player.pos){
            if(e.item.kind==ItemKind::Key && g.opt.auto_pickup_keys){ g.inv.keys++; g.log.add("Picked up a key.");
 g.ents.erase(g.ents.begin()+i);
 return; }
            g.inv.items.push_back(e.item);
 g.log.add("Picked up: "+e.item.name+" ("+item_desc(e.item)+")");
 g.ents.erase(g.ents.begin()+i);
 return;
        }
    } g.log.add("Nothing here to pick up.");
}
static void use_item(Game& g,int idx){
    if(idx<0||idx>=(int)g.inv.items.size()) return; auto it=g.inv.items[idx];
    switch(it.kind){
        case ItemKind::PotionHeal:{ int before=g.player.mob.st.hp; g.player.mob.st.hp=std::min(g.player.mob.st.max_hp,g.player.mob.st.hp+it.power);
 g.log.add("You heal "+std::to_string(g.player.mob.st.hp-before)+" HP.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 if(g.inv.weapon_idx==idx) g.inv.weapon_idx=-1; if(g.inv.armor_idx==idx) g.inv.armor_idx=-1; }break;
        case ItemKind::PotionStr:{ g.player.mob.st.str+=it.power; g.player.mob.st.atk+=it.power/2; g.player.mob.st.max_hp+=it.power; g.player.mob.st.hp=std::min(g.player.mob.st.hp+it.power,g.player.mob.st.max_hp);
 g.log.add("You feel stronger!");
 g.inv.items.erase(g.inv.items.begin()+idx);
 if(g.inv.weapon_idx==idx) g.inv.weapon_idx=-1; if(g.inv.armor_idx==idx) g.inv.armor_idx=-1; }break;
        case ItemKind::PotionAntidote:{ g.player.mob.st.poison=0; g.log.add("Poison cured.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::PotionRegen:{ g.player.mob.st.regen += it.power; g.log.add("You begin regenerating.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::Dagger: case ItemKind::Sword:{ g.inv.weapon_idx=idx; g.log.add("You wield: "+it.name+" (+"+std::to_string(it.power)+")"); }break;
        case ItemKind::ArmorLeather: case ItemKind::ArmorChain:{ g.inv.armor_idx=idx; g.log.add("You don: "+it.name+" (+"+std::to_string(it.power)+")"); }break;
        case ItemKind::Key:{ g.log.add("A key. Use it on a chest with 'o'."); }break;
        case ItemKind::Bomb:{
            // place a timed bomb on the ground (fuse 2 turns)
            Entity b{}; b.type=EntityType::BombPlaced; b.blocks=false; b.pos=g.player.pos; b.fuse=2;
            g.ents.push_back(b);
            g.inv.items.erase(g.inv.items.begin()+idx);
            if(g.inv.weapon_idx==idx) g.inv.weapon_idx=-1;
            if(g.inv.armor_idx==idx) g.inv.armor_idx=-1;
        }break;
        case ItemKind::ScrollMapping:{ for(int r=0;r<g.map.H;r++) for(int c=0;c<g.map.W;c++){ if(g.map.at(r,c).t!=Tile::Wall) g.map.at(r,c).seen=true; } g.log.add("You read the map.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 if(g.inv.weapon_idx==idx) g.inv.weapon_idx=-1; if(g.inv.armor_idx==idx) g.inv.armor_idx=-1; }break;
        case ItemKind::SpellbookFirebolt:{ g.inv.learn(SpellKind::Firebolt);
 g.log.add("Learned Firebolt.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::SpellbookHeal:{ g.inv.learn(SpellKind::Heal);
 g.log.add("Learned Heal.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::SpellbookBlink:{ g.inv.learn(SpellKind::Blink);
 g.log.add("Learned Blink.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::SpellbookIce:{ g.inv.learn(SpellKind::IceShard);
 g.log.add("Learned Ice Shard.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::SpellbookShield:{ g.inv.learn(SpellKind::Shield);
 g.log.add("Learned Shield.");
 g.inv.items.erase(g.inv.items.begin()+idx);
 }break;
        case ItemKind::ScrollBlink:{
            std::vector<Pos> spots; for(int r=0;r<g.map.H;r++) for(int c=0;c<g.map.W;c++) if(g.map.at(r,c).visible && g.map.walkable(r,c) && !(g.player.pos.r==r && g.player.pos.c==c)) spots.push_back({r,c});
            if(spots.empty()) g.log.add("Blink fails.");
 else { g.player.pos=spots[g.rng.i(0,(int)spots.size()-1)]; g.log.add("You blink.");
 }
            g.inv.items.erase(g.inv.items.begin()+idx); if(g.inv.weapon_idx==idx) g.inv.weapon_idx=-1; if(g.inv.armor_idx==idx) g.inv.armor_idx=-1;
        }break;
    }
}
static void drop_item(Game& g,int idx){

    if(idx<0 || idx>=(int)g.inv.items.size()) return;
    Item it = g.inv.items[idx];
    // can't drop onto teleporter or chest occupied tile (also checked by caller)
    for(auto& e: g.ents){
        if(e.pos==g.player.pos && (e.type==EntityType::Chest)){ g.log.add("Can't drop here."); return; }
    }
    Entity ent{}; ent.type=EntityType::ItemEntity; ent.blocks=false; ent.pos=g.player.pos; ent.item=it;
    g.ents.push_back(ent);
    g.inv.items.erase(g.inv.items.begin()+idx);
    if(g.inv.weapon_idx==idx) g.inv.weapon_idx=-1;
    if(g.inv.armor_idx==idx) g.inv.armor_idx=-1;
    if(g.inv.weapon_idx>idx) g.inv.weapon_idx--;
    if(g.inv.armor_idx>idx) g.inv.armor_idx--;
    g.log.add("Dropped "+it.name+".");

}


// ---------------- Doors/Traps/Chests ----------------
static bool is_closed_door(const Map&m,int r,int c){ return m.in(r,c) && m.at(r,c).t==Tile::DoorClosed; }
static void open_door(Game& g,int r,int c){ if(is_closed_door(g.map,r,c)){ g.map.at(r,c).t=Tile::DoorOpen; g.log.add("You open the door."); } }
static TrapKind trap_kind_for_biome(const std::string& biome,RNG&rng){
    if(biome=="Lava Caves") return rng.chance(0.5)?TrapKind::Fire:TrapKind::Explosive;
    if(biome=="Armory") return rng.chance(0.6)?TrapKind::Spike:TrapKind::Snare;
    if(biome=="Sewers") return rng.chance(0.5)?TrapKind::Poison:TrapKind::Teleport;
    if(biome=="Library") return rng.chance(0.6)?TrapKind::Snare:TrapKind::Spike;
    return rng.chance(0.5)?TrapKind::Spike:TrapKind::Snare;
}
static void explode_at(Game& g, int r, int c, int radius){
    g.log.add("An explosion rocks the dungeon!");
    auto in_range = [&](int rr,int cc){ return std::abs(rr-r)+std::abs(cc-c) <= radius; };
    if(in_range(g.player.pos.r, g.player.pos.c)){
        int dmg = g.rng.i(2, 6);
        g.player.mob.st.hp -= dmg;
        g.log.add("You take "+std::to_string(dmg)+" explosive damage!");
    }
    for(auto& e: g.ents){ if(e.type==EntityType::Mob && e.mob.alive && in_range(e.pos.r,e.pos.c)){
        int dmg = g.rng.i(3, 8);
 e.mob.st.hp -= dmg; if(e.mob.st.hp<=0){ e.mob.alive=false; g.log.add(e.mob.name+" is blown apart.");
 grant_xp(g,e.mob.xp);
 g.kills[e.mob.name]++; }
    }}
    for(int rr=r-1; rr<=r+1; ++rr) for(int cc=c-1; cc<=c+1; ++cc){ if(g.map.in(rr,cc) && g.map.at(rr,cc).t==Tile::SecretWall){ g.map.at(rr,cc).t = Tile::DoorOpen; g.map.at(rr,cc).seen=true; g.log.add("A secret wall crumbles!"); } }
}
static void trigger_trap(Game& g,int r,int c){
    g.map.at(r,c).t=Tile::TrapRevealed;
    TrapKind tk=trap_kind_for_biome(g.biome,g.rng);
    switch(tk){
        case TrapKind::Spike:{ int dmg=g.rng.i(2,6);
 g.player.mob.st.hp-=dmg; g.log.add("A spike trap! You take "+std::to_string(dmg)+" damage.");
 }break;
        case TrapKind::Fire:{ g.player.mob.st.burning+=3; g.log.add("A fire trap! You are burning."); }break;
        case TrapKind::Snare:{ g.player.mob.st.snared+=2; g.log.add("A snare! You're entangled."); }break;
        case TrapKind::Poison:{ g.player.mob.st.poison+=4; g.log.add("Poison darts! You are poisoned."); }break;
        case TrapKind::Teleport:{ std::vector<Pos> spots; for(int rr=0;rr<g.map.H;rr++) for(int cc=0;cc<g.map.W;cc++) if(g.map.walkable(rr,cc)) spots.push_back({rr,cc});
 if(!spots.empty()){ g.player.pos = spots[g.rng.i(0,(int)spots.size()-1)]; g.log.add("A teleport trap warps you!");
 } }break;
        case TrapKind::Explosive:{ explode_at(g,r,c,2); }break;
    }
}
static void trigger_trap_on_entity(Game& g, Entity& e, int r, int c){
    g.map.at(r,c).t=Tile::TrapRevealed;
    TrapKind tk=trap_kind_for_biome(g.biome,g.rng);
    switch(tk){
        case TrapKind::Spike:{ int dmg=g.rng.i(2,6); e.mob.st.hp-=dmg; }break;
        case TrapKind::Fire:{ e.mob.st.burning+=3; }break;
        case TrapKind::Snare:{ e.mob.st.snared+=2; }break;
        case TrapKind::Poison:{ e.mob.st.poison+=4; }break;
        case TrapKind::Teleport:{ std::vector<Pos> spots; for(int rr=0;rr<g.map.H;rr++) for(int cc=0;cc<g.map.W;cc++) if(g.map.walkable(rr,cc)) spots.push_back({rr,cc}); if(!spots.empty()){ e.pos = spots[g.rng.i(0,(int)spots.size()-1)]; } }break;
        case TrapKind::Explosive:{ explode_at(g,r,c,2); }break;
    }
}

static void open_chest(Game& g){
    for(size_t i=0;i<g.ents.size();++i){
        auto&e=g.ents[i]; if(e.type==EntityType::Chest && e.pos==g.player.pos){
            if(e.chest.opened){ g.log.add("The chest is empty."); return; }
            if(e.chest.locked){ if(g.inv.keys>0){ g.inv.keys--; e.chest.locked=false; g.log.add("You unlock the chest.");
 } else { g.log.add("Locked. You need a key.");
 return; } }
            e.chest.opened=true; Entity it{}; it.type=EntityType::ItemEntity; it.blocks=false; it.pos=e.pos; it.item=e.chest.content; g.ents.push_back(it);
 g.log.add("You open the chest.");
 return;
        }
    } g.log.add("No chest here.");
}
static void try_open_adjacent(Game& g){
    for(auto nb: neighbors4(g.player.pos.r,g.player.pos.c)){ if(is_closed_door(g.map,nb.r,nb.c)){ open_door(g,nb.r,nb.c); return; } }
    open_chest(g);
}
static void search(Game& g){
    int found=0;
    for(auto nb: neighbors4(g.player.pos.r,g.player.pos.c)){
        if(g.map.in(nb.r,nb.c) && g.map.at(nb.r,nb.c).t==Tile::TrapHidden && g.rng.chance(0.5)){ g.map.at(nb.r,nb.c).t=Tile::TrapRevealed; found++; }
        if(is_closed_door(g.map,nb.r,nb.c) && g.rng.chance(0.25)){ g.log.add("You listen at a door."); }
    }
    if(found>0) g.log.add("You discover "+std::to_string(found)+" trap(s)!");
 else g.log.add("You find nothing.");

}

// --- Color helpers (ANSI) ---
enum class Color:int { Default=0, Wall, Floor, Stairs, Door, Trap, Item, Chest, Mob, Player, Boss, Teleporter, Legend };
static const char* color_code(Color c){
    switch(c){
        case Color::Wall: return "\x1b[38;5;245m";
        case Color::Floor: return "\x1b[38;5;240m";
        case Color::Stairs: return "\x1b[38;5;33m";
        case Color::Door: return "\x1b[38;5;179m";
        case Color::Trap: return "\x1b[38;5;160m";
        case Color::Item: return "\x1b[38;5;213m";
        case Color::Chest: return "\x1b[38;5;178m";
        case Color::Mob: return "\x1b[38;5;208m";
        case Color::Player: return "\x1b[97m";
        case Color::Boss: return "\x1b[38;5;199m";
        case Color::Teleporter: return "\x1b[38;5;45m";
        case Color::Legend: return "\x1b[38;5;250m";
        default: return "\x1b[0m";
    }
}
// ---------------- Rendering ----------------
struct RenderBuf{
    int H,W; std::vector<char> ch; std::vector<Color> col;
    RenderBuf(int h,int w):H(h),W(w),ch(h*w,' '),col(h*w,Color::Default){}
    void set(int r,int c,char g, Color co=Color::Default){ if(r<0||c<0||r>=H||c>=W) return; ch[r*W+c]=g; col[r*W+c]=co; }
    void setcolor(int r,int c, Color co){ if(r<0||c<0||r>=H||c>=W) return; col[r*W+c]=co; }
    void flush(){
        io::move(0,0);
        for(int r=0;r<H;r++){
            Color cur=Color::Default; std::cout << "\x1b[0m";
            for(int c=0;c<W;c++){
                Color wanted = col[r*W+c];
                if(wanted!=cur){ std::cout<<color_code(wanted); cur=wanted; }
                std::cout<<ch[r*W+c];
            }
            std::cout<<"\x1b[0m\n";
        }
    }
};
static bool occupied(const Game& g,int r,int c){
    if(g.player.pos.r==r && g.player.pos.c==c) return true;
    for(auto& e:g.ents) if(e.type!=EntityType::ItemEntity && e.mob.alive && e.blocks && e.pos.r==r && e.pos.c==c) return true;
    return false;
}
static Entity* mob_at(Game& g,int r,int c){ for(auto& e:g.ents) if(e.type==EntityType::Mob && e.mob.alive && e.pos.r==r && e.pos.c==c) return &e; return nullptr; }
static Entity* chest_at(Game& g,int r,int c){ for(auto& e:g.ents) if(e.type==EntityType::Chest && e.pos.r==r && e.pos.c==c) return &e; return nullptr; }
static Entity* item_at(Game& g,int r,int c){ for(auto& e:g.ents) if(e.type==EntityType::ItemEntity && e.pos.r==r && e.pos.c==c) return &e; return nullptr; }

static void draw_hud(const Game& g){

    io::move(g.map.H-4,0);
    int armor=(g.inv.armor_idx>=0 && g.inv.armor_idx<(int)g.inv.items.size())? g.inv.items[g.inv.armor_idx].power:0;

    std::ostringstream stats;
    stats<<"HP "<<g.player.mob.st.hp<<"/"<<g.player.mob.st.max_hp
         <<"  MP "<<g.player.mob.st.mp<<"/"<<g.player.mob.st.max_mp
         <<"  ATK "<<g.player.mob.st.atk
         <<"  DEF "<<g.player.mob.st.def+armor+(g.player.mob.st.shield>0?2:0)
         <<"  STR "<<g.player.mob.st.str
         <<"  L"<<g.level<<"/"<<g.max_level<<" ["<<g.biome<<"]"
         <<"  Keys:"<<g.inv.keys
         <<"  PLv "<<g.plv<<" XP "<<g.xp<<"/"<<xp_to_next(g.plv);

    std::string stats_row = stats.str();

    std::string help = "  (i)nven (g)get (s)earch (o)pen (z)cast (m)ap (X)codex (c)har (O)ptions (>)down (?)help (t)trade (q)save+quit";
    int W = g.map.W;
    std::string row = stats_row;
    int remain = W - (int)row.size();
    if(remain > 0){
        // append truncated help to fit
        if((int)help.size() > remain) help.resize(remain);
        row += help;
    } else if((int)row.size() > W){
        row.resize(W);
    }
    std::cout<< std::left << std::setw(W) << row;

}


static void render(Game& g){
    RenderBuf rb(g.map.H,g.map.W);
    // legend sidebar width
    const int LEG_W = 20;
    int viewW = g.map.W - LEG_W;
    int viewH = std::max(8, g.map.H - 4);

    // camera follow
    if(g.cam_follow){
        g.cam_r = g.player.pos.r - viewH/2;
        g.cam_c = g.player.pos.c - viewW/2;
    }
    if(g.cam_r < 0) g.cam_r = 0;
    if(g.cam_c < 0) g.cam_c = 0;
    if(g.cam_r > g.map.H - viewH) g.cam_r = std::max(0, g.map.H - viewH);
    if(g.cam_c > g.map.W - viewW) g.cam_c = std::max(0, g.map.W - viewW);

    int legend_x = viewW; // screen column where legend starts

    // draw map tiles (viewport)
    for(int sr=0; sr<viewH; ++sr){
        int r = g.cam_r + sr;
        for(int sc=0; sc<viewW; ++sc){
            int c = g.cam_c + sc;
            char ch=' '; Color co=Color::Default;
            if(g.map.in(r,c)){
                const auto& cell=g.map.at(r,c);
                if(cell.visible){
                    ch=tile_glyph(cell);
                    switch(cell.t){
                        case Tile::Wall: co=Color::Wall; break;
                        case Tile::Floor: co=Color::Floor; break;
                        case Tile::StairsDown: co=Color::Stairs; break;
                        case Tile::DoorClosed: case Tile::DoorOpen: co=Color::Door; break;
                        case Tile::TrapHidden: case Tile::TrapRevealed: co=Color::Trap; break;
                        case Tile::SecretWall: co=Color::Wall; break;
                        case Tile::Teleporter: co=Color::Teleporter; break;
                    }
                } else if(cell.seen){
                    ch=(tile_glyph(cell)=='#'?'#':',');
                    co=Color::Legend;
                }
            }
            rb.set(sr,sc,ch,co);
        }
    }

    // entities: items/chests/mobs/player in viewport coords
    auto in_view = [&](int r,int c){ return r>=g.cam_r && r<g.cam_r+viewH && c>=g.cam_c && c<g.cam_c+viewW; };
    auto to_screen = [&](int r,int c){ return Pos{ r - g.cam_r, c - g.cam_c }; };

    
    
    // bombs
    for(auto& e: g.ents){
        if(e.type==EntityType::BombPlaced){
            if(in_view(e.pos.r,e.pos.c)){
                Pos s = to_screen(e.pos.r,e.pos.c);
                rb.set(s.r,s.c,'o', Color::Item);
            }
        }
    }
// merchants
    for(auto& e: g.ents){
        if(e.type==EntityType::Merchant){
            if(in_view(e.pos.r,e.pos.c)){
                Pos s = to_screen(e.pos.r,e.pos.c);
                rb.set(s.r,s.c,'$', Color::Item);
            }
        }
    }
for(auto& e: g.ents){
        if(e.type==EntityType::ItemEntity){
            if(in_view(e.pos.r,e.pos.c) && g.map.at(e.pos.r,e.pos.c).visible){
                Pos s = to_screen(e.pos.r,e.pos.c);
                rb.set(s.r,s.c,e.item.glyph, Color::Item);
            }
        } else if(e.type==EntityType::Chest){
            if(in_view(e.pos.r,e.pos.c) && g.map.at(e.pos.r,e.pos.c).visible){
                Pos s = to_screen(e.pos.r,e.pos.c);
                rb.set(s.r,s.c, e.chest.opened? '=' : '*', Color::Chest);
            }
        }
    }
    for(auto& e: g.ents){
        if(e.type==EntityType::Mob && e.mob.alive && in_view(e.pos.r,e.pos.c)){
            if(g.map.at(e.pos.r,e.pos.c).visible){
                Pos s = to_screen(e.pos.r,e.pos.c);
                rb.set(s.r,s.c, e.mob.glyph, (e.mob.glyph=='B')? Color::Boss: Color::Mob);
            }
        }
    }
    if(in_view(g.player.pos.r,g.player.pos.c)){
        Pos s = to_screen(g.player.pos.r,g.player.pos.c);
        rb.set(s.r,s.c,'@', Color::Player);
    }

    
    // burning zones overlay
    for(size_t i=0;i<g.firezones.size();++i){
        Pos ez = g.firezones[i];
        if(in_view(ez.r,ez.c) && g.map.at(ez.r,ez.c).visible){
            Pos s = to_screen(ez.r,ez.c);
            rb.set(s.r,s.c,'~', Color::Trap);
        }
    }
// legend panel
    for(int r=0;r<g.map.H;r++){
        rb.set(r, legend_x, '|', Color::Legend);
        for(int c=legend_x+1;c<g.map.W;c++){ rb.set(r,c,' ', Color::Legend); }
    }
    auto putL = [&](int row, const char* label, char glyph, Color co){
        if(row>=0 && row<g.map.H){
            rb.set(row, legend_x+1, glyph, co);
            std::string s = std::string(" ")+label;
            for(size_t i=0;i<s.size() && legend_x+3+(int)i<g.map.W;i++) rb.set(row, legend_x+3+i, s[i], Color::Legend);
        }
    };
    int lr=0;
    putL(lr++ , "Player", '@', Color::Player);
    putL(lr++ , "Mob", 'm', Color::Mob);
    putL(lr++ , "Boss", 'B', Color::Boss);
    putL(lr++ , "Wall", '#', Color::Wall);
    putL(lr++ , "Floor", '.', Color::Floor);
    putL(lr++ , "Door", '+', Color::Door);
    putL(lr++ , "Open door", '/', Color::Door);
    putL(lr++ , "Stairs", '>', Color::Stairs);
    putL(lr++ , "Teleporter", 'T', Color::Teleporter);
    putL(lr++ , "Trap", '^', Color::Trap);
    putL(lr++ , "Item", '!', Color::Item);
    putL(lr++ , "Chest", '*', Color::Chest);
    putL(lr++ , "Merchant", '$', Color::Item);

    io::clear();
    rb.flush();
    draw_hud(g);
    g.log.render(g.map.H,g.map.W);
    io::flush();
}
static void show_help(){

    io::move(0,0); io::clear();
    std::cout<<"Help\n";
    std::cout<<"Move: arrows or W/A/D (S=down). '.' wait\n";
    std::cout<<"Camera: H/J/K/L pan, F toggle follow\n";
    std::cout<<"Actions: g get, i inventory (x drop), s search, o open, z cast, m map, X codex, c char, O options, t trade (near $), > teleporter, ? help, q save+quit\n\n";
    std::cout<<"Legend: @ you, m mob, B boss, # wall, . floor, +/ door, ^ trap, * chest, = opened, T teleporter, $ merchant, ~ burning\n";
    std::cout<<"Spells show cost and effects; re-reading books improves them. Blink teleports to a selected visible tile.\n";
    std::cout<<"Opening menus (inventory/map/codex/options/help) doesn't pass time.\n";
    std::cout<<"Press any key...\n";
    io::flush(); (void)io::getch_blocking();

}


static void inventory_modal(Game& g){

    while(true){
        io::move(0,0);
        io::clear();
        std::cout<<"Inventory\n";
        for(size_t i=0;i<g.inv.items.size();++i){
            const auto& it=g.inv.items[i];
            std::cout<<"  "<<(char)('a'+i)<<") "<<it.name<<" - "<<item_desc(it);
            if((int)i==g.inv.weapon_idx) std::cout<<" [weapon]";
            if((int)i==g.inv.armor_idx) std::cout<<" [armor]";
            std::cout<<"\n";
        }
        if(g.inv.items.empty()) std::cout<<"  (empty)\n";
        std::cout<<"Keys: "<<g.inv.keys<<"   Gold: "<<g.gold<<"\n";
        std::cout<<"Use: letter, (x) drop, (q) quit\n> "<<std::flush;
        int ch = io::getch_blocking();
        if(ch=='q') break;
        if(ch=='x'){
            std::cout<<"\nDrop which? (letter) > "<<std::flush;
            int x=io::getch_blocking(); int idx=x-'a';
            if(idx>=0 && idx<(int)g.inv.items.size()){
                // don't drop if tile already has item or chest
                bool blocked=false;
                for(auto& e: g.ents){
                    if(e.pos==g.player.pos && (e.type==EntityType::ItemEntity || e.type==EntityType::Chest)){ blocked=true; break; }
                }
                if(blocked){ g.log.add("Too cluttered to drop here."); }
                else{
                    drop_item(g,idx);
                    // turn passes when you drop
                    ai_turn(g); process_statuses(g); world_tick(g);
                }
            }
            continue;
        }
        int idx = ch - 'a';
        if(idx>=0 && idx<(int)g.inv.items.size()){
            use_item(g,idx);
            // turn passes on use
            ai_turn(g); process_statuses(g); world_tick(g);
        }
    }

}

static void options_modal(Game& g){
    while(true){
        io::move(0,0);
 io::clear();

        std::cout<<"Options:\n";
        std::cout<<"  1) Auto open door on bump: "<<(g.opt.auto_open_on_bump?"ON":"OFF")<<"\n";
        std::cout<<"  2) Auto pickup keys: "<<(g.opt.auto_pickup_keys?"ON":"OFF")<<"\n";
        std::cout<<"  q) Back\n> "<<std::flush;
        int ch=io::getch_blocking(); if(ch=='q'||ch==27) break;
        if(ch=='1') g.opt.auto_open_on_bump=!g.opt.auto_open_on_bump;
        if(ch=='2') g.opt.auto_pickup_keys=!g.opt.auto_pickup_keys;
    }
}
static void character_modal(Game& g){
    io::move(0,0);
 io::clear();

    auto& st=g.player.mob.st; int armor=(g.inv.armor_idx>=0 && g.inv.armor_idx<(int)g.inv.items.size())? g.inv.items[g.inv.armor_idx].power:0;
    std::cout<<"Character Sheet\n\n";
    std::cout<<"Level: "<<g.plv<<"  XP: "<<g.xp<<"/"<<xp_to_next(g.plv)<<"\n";
    std::cout<<"HP: "<<st.hp<<"/"<<st.max_hp<<"   MP: "<<st.mp<<"/"<<st.max_mp<<"\n";
    std::cout<<"ATK: "<<st.atk<<"   DEF: "<<st.def+armor+(st.shield>0?2:0)<<"   STR: "<<st.str<<"\n";
    std::cout<<"Statuses: burn "<<st.burning<<", poison "<<st.poison<<", regen "<<st.regen<<", snare "<<st.snared<<", shield "<<st.shield<<"\n\n";
    std::cout<<"Press any key...\n"; io::flush();
 (void)io::getch_blocking();

}
static void codex_modal(Game& g){
    io::move(0,0);
 io::clear();

    std::cout<<"Codex (kills):\n";
    if(g.kills.empty()) std::cout<<"  (empty)\n"; else { for(auto& kv: g.kills) std::cout<<"  "<<kv.first<<": "<<kv.second<<"\n"; }
    std::cout<<"\nPress any key...\n"; io::flush();
 (void)io::getch_blocking();

}
static void map_modal(Game& g){

    io::move(0,0);
    io::clear();
    RenderBuf rb(g.map.H,g.map.W);
    for(int r=0;r<g.map.H;r++){
        for(int c=0;c<g.map.W;c++){
            const auto& cell=g.map.at(r,c);
            char ch=' ';
            if(cell.seen) ch=tile_glyph(cell);
            rb.set(r,c,ch, Color::Legend);
            if(cell.seen && cell.t==Tile::TrapRevealed){
                rb.set(r,c,'^', Color::Trap);
            }
        }
    }
    // chests on seen tiles
    for(auto& e: g.ents){
        if(e.type==EntityType::Chest){
            if(g.map.at(e.pos.r,e.pos.c).seen){
                rb.set(e.pos.r,e.pos.c, e.chest.opened? '=' : '*', Color::Chest);
            }
        }
    }
    // player position
    rb.set(g.player.pos.r,g.player.pos.c,'@', Color::Player);
    io::clear();
    rb.flush();
    std::cout << "\n(Seen map) Press any key...\n";
    io::flush();
    (void)io::getch_blocking();

}
static int to_int(Tile t){ return (int)t; } static Tile to_tile(int v){ return (Tile)v; }
static void save_game(const Game& g){
    std::ofstream f("savegame.txt"); if(!f) return;
    f<<"LEVEL "<<g.level<<" "<<g.map.H<<" "<<g.map.W<<" "<<g.plv<<" "<<g.xp<<" "<<g.opt.auto_open_on_bump<<" "<<g.opt.auto_pickup_keys<<"\n";
    f<<"PR "<<g.player.pos.r<<" "<<g.player.pos.c<<" "<<g.player.mob.st.hp<<" "<<g.player.mob.st.max_hp<<" "<<g.player.mob.st.atk<<" "<<g.player.mob.st.def<<" "<<g.player.mob.st.str<<" "<<g.player.mob.st.mp<<" "<<g.player.mob.st.max_mp<<" "<<g.player.mob.st.burning<<" "<<g.player.mob.st.snared<<" "<<g.player.mob.st.poison<<" "<<g.player.mob.st.regen<<" "<<g.player.mob.st.shield<<"\n";
    f<<"IN "<<g.inv.keys<<" "<<g.inv.weapon_idx<<" "<<g.inv.armor_idx<<" "<<g.inv.items.size()<<"\n";
    for(auto& it: g.inv.items) f<<"IT "<<(int)it.kind<<" "<<it.name<<"| "<<(int)it.glyph<<" "<<it.power<<"\n";
    f<<"SP "<<g.inv.spells.size()<<"\n"; for(auto s: g.inv.spells) f<<(int)s<<"\n";
    f<<"KILL "<<g.kills.size()<<"\n"; for(auto& kv: g.kills) f<<kv.first<<"| "<<kv.second<<"\n";
    f<<"EN "<<g.ents.size()<<"\n";
    for(auto& e: g.ents){
        if(e.type==EntityType::Mob){
            f<<"MOB "<<e.pos.r<<" "<<e.pos.c<<" "<<(int)e.mob.alive<<" "<<e.mob.name<<"| "<<(int)e.mob.glyph<<" "<<e.mob.st.max_hp<<" "<<e.mob.st.hp<<" "<<e.mob.st.atk<<" "<<e.mob.st.def<<" "<<e.mob.st.str<<" "<<e.mob.xp<<"\n";
        } else if(e.type==EntityType::ItemEntity){
            f<<"ITM "<<e.pos.r<<" "<<e.pos.c<<" "<<(int)e.item.kind<<" "<<e.item.name<<"| "<<(int)e.item.glyph<<" "<<e.item.power<<"\n";
        } else if(e.type==EntityType::Chest){
            f<<"CHS "<<e.pos.r<<" "<<e.pos.c<<" "<<(int)e.chest.locked<<" "<<(int)e.chest.opened<<" "<<(int)e.chest.content.kind<<" "<<e.chest.content.name<<"| "<<(int)e.chest.content.glyph<<" "<<e.chest.content.power<<"\n";
        }
    }
    f<<"MP "<<g.map.H<<"\n";
    for(int r=0;r<g.map.H;r++){ for(int c=0;c<g.map.W;c++){ f<<to_int(g.map.at(r,c).t)<<" "<<(g.map.at(r,c).seen?1:0)<<" "; } f<<"\n"; }
}
static bool load_game(Game& g){
    std::ifstream f("savegame.txt"); if(!f) return false; std::string tag; int H,W;
    f>>tag>>g.level>>H>>W>>g.plv>>g.xp>>g.opt.auto_open_on_bump>>g.opt.auto_pickup_keys; if(tag!="LEVEL") return false; g.map=Map(H,W);
    int r,c; f>>tag>>r>>c>>g.player.mob.st.hp>>g.player.mob.st.max_hp>>g.player.mob.st.atk>>g.player.mob.st.def>>g.player.mob.st.str>>g.player.mob.st.mp>>g.player.mob.st.max_mp>>g.player.mob.st.burning>>g.player.mob.st.snared>>g.player.mob.st.poison>>g.player.mob.st.regen>>g.player.mob.st.shield; g.player.pos={r,c};
    int nitems; f>>tag>>g.inv.keys>>g.inv.weapon_idx>>g.inv.armor_idx>>nitems; g.inv.items.clear();
 std::string line; std::getline(f,line);

    for(int i=0;i<nitems;i++){ std::getline(f,line);
 std::istringstream ss(line);
 std::string itag; ss>>itag; int kind,glyph,power; std::string namepipe; ss>>kind>>namepipe>>glyph>>power; if(!namepipe.empty()&&namepipe.back()=='|') namepipe.pop_back();
 Item it; it.kind=(ItemKind)kind; it.name=namepipe; it.glyph=(char)glyph; it.power=power; g.inv.items.push_back(it);
 }
    int nsp; f>>tag>>nsp; g.inv.spells.clear();
 for(int i=0;i<nsp;i++){ int s; f>>s; g.inv.spells.push_back((SpellKind)s);
 }
    int nk; f>>tag>>nk; g.kills.clear();
 std::getline(f,line);
 for(int i=0;i<nk;i++){ std::getline(f,line);
 std::istringstream ss(line);
 std::string namepipe; int cnt; ss>>namepipe>>cnt; if(!namepipe.empty()&&namepipe.back()=='|') namepipe.pop_back();
 g.kills[namepipe]=cnt; }
    int nents; f>>tag>>nents; std::getline(f,line);
 g.ents.clear();

    for(int i=0;i<nents;i++){ std::getline(f,line);
 std::istringstream ss(line);
 std::string et; ss>>et;
        if(et=="MOB"){ Entity e{}; e.type=EntityType::Mob; int alive,glyph; ss>>e.pos.r>>e.pos.c>>alive; e.mob.alive=alive!=0; std::string namepipe; ss>>namepipe>>glyph>>e.mob.st.max_hp>>e.mob.st.hp>>e.mob.st.atk>>e.mob.st.def>>e.mob.st.str>>e.mob.xp; if(!namepipe.empty()&&namepipe.back()=='|') namepipe.pop_back();
 e.mob.name=namepipe; e.mob.glyph=(char)glyph; g.ents.push_back(e);
 }
        else if(et=="ITM"){ Entity e{}; e.type=EntityType::ItemEntity; e.blocks=false; int kind,glyph,power; ss>>e.pos.r>>e.pos.c>>kind; std::string namepipe; ss>>namepipe>>glyph>>power; if(!namepipe.empty()&&namepipe.back()=='|') namepipe.pop_back();
 e.item.kind=(ItemKind)kind; e.item.name=namepipe; e.item.glyph=(char)glyph; e.item.power=power; g.ents.push_back(e);
 }
        else if(et=="CHS"){ Entity e{}; e.type=EntityType::Chest; e.blocks=true; int locked,opened,kind,glyph,power; ss>>e.pos.r>>e.pos.c>>locked>>opened>>kind; std::string namepipe; ss>>namepipe>>glyph>>power; if(!namepipe.empty()&&namepipe.back()=='|') namepipe.pop_back();
 e.chest.locked=locked!=0; e.chest.opened=opened!=0; e.chest.content.kind=(ItemKind)kind; e.chest.content.name=namepipe; e.chest.content.glyph=(char)glyph; e.chest.content.power=power; g.ents.push_back(e);
 }
    }
    int Hhdr; f>>tag>>Hhdr; std::getline(f,line);
 for(int rr=0; rr<g.map.H; rr++){ std::getline(f,line);
 std::istringstream ss(line);
 for(int cc=0; cc<g.map.W; cc++){ int t,seen; ss>>t>>seen; g.map.at(rr,cc).t=to_tile(t);
 g.map.at(rr,cc).seen=(seen!=0);
 } }
    return true;
}

// ---------------- Input/Turns ----------------
enum class CmdType{ Move,Wait,Pickup,Inventory,Descend,SaveQuit,NewGame,LoadGame,Help,Search,Open,Cast,Map,Codex,Char,Options,CamPan,CamToggle,Trade,None };
struct Cmd{ CmdType type=CmdType::None; int dr=0,dc=0; };


static Cmd read_cmd(){
    int ch = io::getch_blocking();

    // common commands
    if(ch=='q') return {CmdType::SaveQuit,0,0};
    if(ch=='n') return {CmdType::NewGame,0,0};
    if(ch=='r') return {CmdType::LoadGame,0,0};
    if(ch=='?') return {CmdType::Help,0,0};
    if(ch=='.') return {CmdType::Wait,0,0};
    if(ch=='g') return {CmdType::Pickup,0,0};
    if(ch=='i') return {CmdType::Inventory,0,0};
    if(ch=='s') return {CmdType::Search,0,0}; // keep 's' for search
    if(ch=='o') return {CmdType::Open,0,0};
    if(ch=='z') return {CmdType::Cast,0,0};
    if(ch=='m') return {CmdType::Map,0,0};
    if(ch=='X') return {CmdType::Codex,0,0};
    if(ch=='c' || ch=='C') return {CmdType::Char,0,0};
    if(ch=='O') return {CmdType::Options,0,0};
    if(ch=='t' || ch=='T') return {CmdType::Trade,0,0};
    if(ch=='F') return {CmdType::CamToggle,0,0};
    if(ch=='>') return {CmdType::Descend,0,0};

    // camera pan (free camera): HJKL
    if(ch=='H') return {CmdType::CamPan,0,-1};
    if(ch=='J') return {CmdType::CamPan,1,0};
    if(ch=='K') return {CmdType::CamPan,-1,0};
    if(ch=='L') return {CmdType::CamPan,0,1};

    // movement by WASD (uppercase and lowercase), but keep 's' for Search (uppercase S moves down)
    if(ch=='w' || ch=='W') return {CmdType::Move,-1,0};
    if(ch=='a' || ch=='A') return {CmdType::Move,0,-1};
    if(ch=='d' || ch=='D') return {CmdType::Move,0,1};
    if(ch=='S') return {CmdType::Move,1,0};

#ifdef _WIN32
    // Windows: arrow keys come as 0 or 224 followed by code
    if(ch==0 || ch==224){
        int ch2 = io::getch_blocking();
        if(ch2==72) return {CmdType::Move,-1,0}; // up
        if(ch2==80) return {CmdType::Move, 1,0}; // down
        if(ch2==75) return {CmdType::Move, 0,-1}; // left
        if(ch2==77) return {CmdType::Move, 0, 1}; // right
    }
#else
    // POSIX: ESC [ A/B/C/D
    if(ch==27){
        int ch1 = io::getch_blocking();
        if(ch1=='['){
            int ch2 = io::getch_blocking();
            if(ch2=='A') return {CmdType::Move,-1,0};
            if(ch2=='B') return {CmdType::Move, 1,0};
            if(ch2=='D') return {CmdType::Move, 0,-1};
            if(ch2=='C') return {CmdType::Move, 0, 1};
        }
    }
#endif
    return {CmdType::None,0,0};
}



static void move_or_attack(Game& g,int dr,int dc){
    if(g.player.mob.st.snared>0){ g.log.add("You are snared!"); return; }
    int nr=g.player.pos.r+dr, nc=g.player.pos.c+dc; if(!g.map.in(nr,nc)) return;
    if(is_closed_door(g.map,nr,nc) && g.opt.auto_open_on_bump){ open_door(g,nr,nc); return; }
    if(g.map.at(nr,nc).t==Tile::TrapHidden){ trigger_trap(g,nr,nc); g.player.pos={nr,nc}; return; }
    if(Entity* m=mob_at(g,nr,nc)){ attack(g,g.player,*m,"You",m->mob.name); return; }
    if(g.map.walkable(nr,nc) && !occupied(g,nr,nc)) g.player.pos={nr,nc};
}


static void ai_turn(Game& g){
    for(auto& e: g.ents){
        if(e.type!=EntityType::Mob || !e.mob.alive) continue;
        // accumulate energy
        e.mob.energy += e.mob.speed;
        int steps = 0;
        while(e.mob.energy >= 100 && steps < 3){
            // snared: consume a turn doing nothing
            if(e.mob.st.snared>0){ e.mob.st.snared--; e.mob.energy -= 100; steps++; continue; }

            // act
            if(e.mob.ai==AiKind::Wander){
                int dir=g.rng.i(0,4); int dr[5]={-1,1,0,0,0}, dc[5]={0,0,-1,1,0}; int nr=e.pos.r+dr[dir], nc=e.pos.c+dc[dir];
                if(g.player.pos.r==nr && g.player.pos.c==nc) attack(g,e,g.player,e.mob.name,"You");
                else if(g.map.walkable(nr,nc) && !occupied(g,nr,nc)) { if(g.map.at(nr,nc).t==Tile::TrapHidden) trigger_trap_on_entity(g,e,nr,nc); e.pos={nr,nc}; }
            } else {
                if(g.map.at(e.pos.r,e.pos.c).visible){
                    auto path=astar(g.map,e.pos,g.player.pos);
                    if(path.size()>=2){
                        Pos step=path[1];
                        if(step==g.player.pos) attack(g,e,g.player,e.mob.name,"You");
                        else if(!occupied(g,step.r,step.c)) { if(g.map.at(step.r,step.c).t==Tile::TrapHidden) trigger_trap_on_entity(g,e,step.r,step.c); e.pos=step; }
                    }
                } else if(g.rng.chance(0.3)){
                    int dir=g.rng.i(0,4); int dr[5]={-1,1,0,0,0}, dc[5]={0,0,-1,1,0};
                    int nr=e.pos.r+dr[dir], nc=e.pos.c+dc[dir];
                    if(g.map.walkable(nr,nc) && !occupied(g,nr,nc)) e.pos={nr,nc};
                }
            }
            e.mob.energy -= 100; steps++;
        }
    }
}


// ---------------- Tips loader ----------------
static void maybe_tip_from_file(Game& g){
    std::ifstream f("tips.txt");
 if(!f) return; std::vector<std::string> tips; std::string s; while(std::getline(f,s)){ if(!s.empty()) tips.push_back(s);
 if(tips.size()>1000) break; }
    if(!tips.empty() && g.rng.chance(0.35)) g.log.add(tips[g.rng.i(0,(int)tips.size()-1)]);
}

// ---------------- Setup ----------------
static void init_player(Game& g){ g.player.type=EntityType::Player; g.player.blocks=true; g.player.mob.name="You"; g.player.mob.glyph='@'; g.player.mob.st={20,20,3,1,10, 12,12, 0,0,0,0,0}; g.inv=Inventory{}; g.plv=1; g.xp=0; }
static void add_secret_rooms(Game& g){
    int rooms = g.rng.i(1,2);
    for(int k=0;k<rooms;k++){
        int h=g.rng.i(3,5), w=g.rng.i(3,5);
        int r=g.rng.i(2, g.map.H-h-3);
        int c=g.rng.i(2, g.map.W-w-23);
        bool ok=true;
        for(int rr=r-1; rr<r+h+1; rr++) for(int cc=c-1; cc<c+w+1; cc++){ if(!g.map.in(rr,cc) || g.map.at(rr,cc).t!=Tile::Wall){ ok=false; break; } if(!ok) break; }
        if(!ok) continue;
        for(int rr=r; rr<r+h; rr++) for(int cc=c; cc<c+w; cc++) g.map.at(rr,cc).t=Tile::Floor;
        for(int rr=r-1; rr<r+h+1; rr++){ g.map.at(rr,c-1).t=Tile::SecretWall; g.map.at(rr,c+w).t=Tile::SecretWall; }
        for(int cc=c-1; cc<c+w+1; cc++){ g.map.at(r-1,cc).t=Tile::SecretWall; g.map.at(r+h,cc).t=Tile::SecretWall; }
        Entity ch{}; ch.type=EntityType::Chest; ch.blocks=false; ch.pos={r+h/2, c+w/2}; ch.chest.locked=g.rng.chance(0.5);
 ch.chest.opened=false; ch.chest.content=make_random_item(g.rng);
 g.ents.push_back(ch);

    }
}

static void new_level(Game& g){ g.ents.clear();
 auto rooms=generate_dungeon(g.map,g.rng,g.biome);
 place_player(g,rooms);
 place_mobs_items_chests(g,rooms);
    // maybe place merchant near first room center
    if(g.rng.chance(0.25) && !rooms.empty()){
        Pos c{ rooms[0].r + rooms[0].h/2, rooms[0].c + rooms[0].w/2 };
        Entity m{}; m.type=EntityType::Merchant; m.blocks=false; m.pos=c;
        g.ents.push_back(m);
    }


    add_secret_rooms(g);
    // record teleporter position
    g.teleporter = {-1,-1};
    for(int r=0;r<g.map.H;r++) for(int c=0;c<g.map.W;c++) if(g.map.at(r,c).t==Tile::Teleporter) g.teleporter = {r,c};
    if(g.teleporter.r<0 && !rooms.empty()){
        // choose farthest room center from player
        Pos best = Pos{ rooms[0].r + rooms[0].h/2, rooms[0].c + rooms[0].w/2 };
        int bestd = -1;
        for(auto& R: rooms){
            Pos cc{ R.r + R.h/2, R.c + R.w/2 };
            int d = std::abs(cc.r - g.player.pos.r) + std::abs(cc.c - g.player.pos.c);
            if(d > bestd){ bestd=d; best=cc; }
        }
        if(g.map.walkable(best.r,best.c)){ g.map.at(best.r,best.c).t = Tile::Teleporter; g.teleporter = best; }
    }

    // spawn boss near teleporter
    if(g.teleporter.r>=0){
        Entity boss{}; boss.type=EntityType::Mob; boss.blocks=true; boss.pos = {g.teleporter.r + (g.rng.i(0,1)?1:-1), g.teleporter.c};
        boss.mob.name="Guardian"; boss.mob.glyph='B'; boss.mob.st.max_hp=boss.mob.st.hp=28 + g.level*4;
        boss.mob.st.atk=6 + g.level; boss.mob.st.def=3 + g.level/2; boss.mob.st.str=14 + g.level;
        boss.mob.ai=AiKind::Hunter; boss.mob.alive=true; boss.mob.xp=20 + g.level*5;
        if(g.map.walkable(boss.pos.r,boss.pos.c)) g.ents.push_back(boss);
    }
 g.log.add("You descend to level "+std::to_string(g.level)+" ["+g.biome+"].");
 maybe_tip_from_file(g);
 compute_fov(g.map,g.player.pos.r,g.player.pos.c,10);
 }
static void next_level(Game& g){ if(g.level>=g.max_level){ g.log.add("You reach the bottom. Victory!");
 g.running=false; return; } g.level++; new_level(g);
 }
static void new_game(Game& g){ g.level=1; init_player(g);
 new_level(g);
 g.log.add("Welcome!");
 }

// ---------------- Main ----------------

// ---------------- Spells ----------------
static bool los_clear(const Map& m,Pos a,Pos b){
    int r=a.r,c=a.c; int dr=(b.r>r?1:(b.r<r?-1:0)), dc=(b.c>c?1:(b.c<c?-1:0));
    while(r!=b.r || c!=b.c){ r+=dr; c+=dc; if(opaque(m,r,c)) return false; }
    return true;
}
static void cast_firebolt(Game& g,int dr,int dc){
    int fb_boost=g.inv.boost(SpellKind::Firebolt); int fb_cost=std::max(1,3 - fb_boost); if(g.player.mob.st.mp<fb_cost){ g.log.add("Not enough MP ("+std::to_string(fb_cost)+")."); return; } g.player.mob.st.mp-=fb_cost;
    int r=g.player.pos.r,c=g.player.pos.c;
    while(true){ r+=dr; c+=dc; if(!g.map.in(r,c) || opaque(g.map,r,c)) break;
        for(auto& e:g.ents) if(e.type==EntityType::Mob && e.mob.alive && e.pos.r==r && e.pos.c==c){
            int dmg=4+g.rng.i(0,3)+fb_boost;
 e.mob.st.hp-=dmg; e.mob.st.burning+=2; g.log.add("Firebolt hits "+e.mob.name+" for "+std::to_string(dmg)+"!");
 if(e.mob.st.hp<=0){ e.mob.alive=false; g.log.add(e.mob.name+" dies.");
 grant_xp(g,e.mob.xp);
 g.kills[e.mob.name]++; level_up(g);
} return; }
    } g.log.add("The firebolt fizzles."); }
static void cast_heal(Game& g){ if(g.player.mob.st.mp<4){ g.log.add("Not enough MP (4).");
 return; } g.player.mob.st.mp-=4; int before=g.player.mob.st.hp; g.player.mob.st.hp=std::min(g.player.mob.st.max_hp,g.player.mob.st.hp+6);
 g.log.add("You cast Heal ("+std::to_string(g.player.mob.st.hp-before)+" HP).");
 }
static void cast_blink(Game& g){

    int b_boost=g.inv.boost(SpellKind::Blink); int b_cost=std::max(3,6 - b_boost);
    if(g.player.mob.st.mp<b_cost){ g.log.add("Not enough MP ("+std::to_string(b_cost)+")."); return; } 
    Pos tgt; if(!target_tile(g,20,tgt)){ g.log.add("Cancelled."); return; }
    // must be visible and walkable
    if(!g.map.in(tgt.r,tgt.c) || !g.map.at(tgt.r,tgt.c).visible || !g.map.walkable(tgt.r,tgt.c)){ g.log.add("Cannot blink there."); return; }
    g.player.mob.st.mp -= b_cost;
    g.player.pos = tgt;
    g.log.add("You blink.");

}

static void cast_ice(Game& g,Pos target){
    int i_boost=g.inv.boost(SpellKind::IceShard); int i_cost=std::max(2,4 - i_boost); if(g.player.mob.st.mp<i_cost){ g.log.add("Not enough MP ("+std::to_string(i_cost)+")."); return; } g.player.mob.st.mp-=i_cost;
    if(!g.map.in(target.r,target.c) || !los_clear(g.map,g.player.pos,target)){ g.log.add("No line of sight."); return; }
    for(auto& e:g.ents) if(e.type==EntityType::Mob && e.mob.alive && e.pos==target){
        int dmg=3+g.rng.i(0,2);
 e.mob.st.hp-=dmg; e.mob.st.snared+=2; g.log.add("Ice shard hits "+e.mob.name+" ("+std::to_string(dmg)+").");
 if(e.mob.st.hp<=0){ e.mob.alive=false; g.log.add(e.mob.name+" dies.");
 grant_xp(g,e.mob.xp);
 g.kills[e.mob.name]++; level_up(g);
} return; }
    g.log.add("The shard shatters harmlessly.");
}
static void cast_shield(Game& g){ if(g.player.mob.st.mp<3){ g.log.add("Not enough MP (3).");
 return; } g.player.mob.st.mp-=3; g.player.mob.st.shield=5; g.log.add("A shimmering shield surrounds you (+2 DEF for 5 turns).");
 }

// Targeting UI
static bool target_tile(Game& g,int range,Pos& out){

    Pos cur = g.player.pos;
    while(true){
        compute_fov(g.map,g.player.pos.r,g.player.pos.c,10);
        render(g);
        // overlay cursor 'X' at screen coords if in view
        int sr = cur.r - g.cam_r;
        int sc = cur.c - g.cam_c;
        if(sr>=0 && sr<g.map.H && sc>=0 && sc<g.map.W){
            io::move(sr, sc);
            std::cout << "\x1b[97mX\x1b[0m";
        }
        io::flush();
        int ch = io::getch_blocking();
        if(ch==27) return false;
        if(ch=='\n' || ch=='\r'){ out=cur; return true; }
        int dr=0, dc=0;
        if(ch=='w'||ch=='W') dr=-1;
        else if(ch=='s'||ch=='S') dr=1;
        else if(ch=='a'||ch=='A') dc=-1;
        else if(ch=='d'||ch=='D') dc=1;
#ifdef _WIN32
        if(ch==0||ch==224){
            int ch2=io::getch_blocking();
            if(ch2==72) dr=-1;
            if(ch2==80) dr=1;
            if(ch2==75) dc=-1;
            if(ch2==77) dc=1;
        }
#else
        if(ch==27){
            int ch1=io::getch_blocking();
            if(ch1=='['){
                int ch2=io::getch_blocking();
                if(ch2=='A') dr=-1;
                if(ch2=='B') dr=1;
                if(ch2=='D') dc=-1;
                if(ch2=='C') dc=1;
            }
        }
#endif
        Pos nxt{cur.r+dr, cur.c+dc};
        int dist = std::abs(nxt.r - g.player.pos.r) + std::abs(nxt.c - g.player.pos.c);
        if(g.map.in(nxt.r,nxt.c) && dist<=range){
            cur = nxt;
        }
    }

}
static void cast_fireball(Game& g, Pos target){
    int boost = g.inv.boost(SpellKind::Fireball);
    int cost = std::max(2, 6 - boost);
    if(g.player.mob.st.mp < cost){ g.log.add("Not enough MP ("+std::to_string(cost)+")."); return; }
    g.player.mob.st.mp -= cost;
    if(!g.map.in(target.r,target.c) || !los_clear(g.map,g.player.pos,target)){ g.log.add("No line of sight."); return; }
    int radius = 2 + (boost>=3?1:0);
    auto inR = [&](int r,int c){ return std::abs(r-target.r)+std::abs(c-target.c) <= radius; };
    if(inR(g.player.pos.r,g.player.pos.c)){ int dmg= g.rng.i(2,4) + boost; g.player.mob.st.hp -= dmg; g.player.mob.st.burning += 2; }
    for(auto& e: g.ents){
        if(e.type==EntityType::Mob && e.mob.alive && inR(e.pos.r,e.pos.c)){
            int dmg= g.rng.i(4,7) + boost;
            e.mob.st.hp -= dmg; e.mob.st.burning += 2;
            if(e.mob.st.hp<=0){ e.mob.alive=false; g.log.add(e.mob.name+" is incinerated."); grant_xp(g,e.mob.xp); g.kills[e.mob.name]++; }
        }
    }
    for(int r=target.r-radius; r<=target.r+radius; ++r){
        for(int c=target.c-radius; c<=target.c+radius; ++c){
            if(!g.map.in(r,c)) continue;
            if(inR(r,c) && g.map.walkable(r,c)){
                g.firezones.push_back({r,c});
                g.firettl.push_back(3 + boost);
            }
        }
    }
    g.log.add("You cast Fireball.");
}
static void spell_menu(Game& g){

    while(true){
        io::move(0,0); io::clear();
        std::cout<<"Cast spell (MP "<<g.player.mob.st.mp<<"/"<<g.player.mob.st.max_mp<<")\n\n";
        for(size_t i=0;i<g.inv.spells.size();++i){
            auto s=g.inv.spells[i];
            std::string name; int boost=g.inv.boost(s);
            switch(s){
                case SpellKind::Firebolt:{ int cost=std::max(1,3-boost); int dmg=4+boost; name="Firebolt ["+std::to_string(cost)+"MP] dmg~"+std::to_string(dmg)+" (dir)"; }break;
                case SpellKind::Heal:{ int cost=std::max(2,4-boost); int hp=6+boost; name="Heal ["+std::to_string(cost)+"MP] heal "+std::to_string(hp); }break;
                case SpellKind::Blink:{ int cost=std::max(3,6-boost); name="Blink ["+std::to_string(cost)+"MP] to visible"; }break;
                case SpellKind::IceShard:{ int cost=std::max(2,4-boost); int dmg=3+boost; name="Ice Shard ["+std::to_string(cost)+"MP] dmg~"+std::to_string(dmg)+" + snare"; }break;
                case SpellKind::Shield:{ int cost=std::max(1,3-boost); int dur=5+boost; name="Shield ["+std::to_string(cost)+"MP] +DEF ("+std::to_string(dur)+"t)"; }break;
                case SpellKind::Fireball:{ int cost=std::max(2,6-boost); int rad=2+(boost>=3?1:0); name="Fireball ["+std::to_string(cost)+"MP] AoE r="+std::to_string(rad)+" burn"; }break;
            }
            std::cout<<"  "<<(char)('a'+i)<<") "<<name<<"\n";
        }
        if(g.inv.spells.empty()) std::cout<<"  (no spells)\n";
        std::cout<<"\n(letter casts, q quits)\n> "<<std::flush;
        int ch=io::getch_blocking();
        if(ch=='q') break;
        int idx=ch-'a';
        if(idx>=0 && idx<(int)g.inv.spells.size()){
            auto s=g.inv.spells[idx];
            if(s==SpellKind::Firebolt){
                std::cout<<"Direction (WASD/arrows) > "<<std::flush;
                int dr=0,dc=0; int k=io::getch_blocking();
                if(k=='w'||k=='W') dr=-1; else if(k=='s'||k=='S') dr=1; else if(k=='a'||k=='A') dc=-1; else if(k=='d'||k=='D') dc=1;
#ifdef _WIN32
                if(k==0||k==224){ int k2=io::getch_blocking(); if(k2==72) dr=-1; if(k2==80) dr=1; if(k2==75) dc=-1; if(k2==77) dc=1; }
#else
                if(k==27){ int k1=io::getch_blocking(); if(k1=='['){ int k2=io::getch_blocking(); if(k2=='A') dr=-1; if(k2=='B') dr=1; if(k2=='D') dc=-1; if(k2=='C') dc=1; } }
#endif
                if(dr!=0 || dc!=0){ cast_firebolt(g,dr,dc); ai_turn(g); process_statuses(g); world_tick(g); }
            } else if(s==SpellKind::Heal){
                cast_heal(g); ai_turn(g); process_statuses(g); world_tick(g);
            } else if(s==SpellKind::Blink){
                cast_blink(g); ai_turn(g); process_statuses(g); world_tick(g);
            } else if(s==SpellKind::IceShard){
                Pos tgt; if(target_tile(g,6,tgt)){ cast_ice(g,tgt); ai_turn(g); process_statuses(g); world_tick(g); }
            } else if(s==SpellKind::Shield){
                cast_shield(g); ai_turn(g); process_statuses(g); world_tick(g);
            } else if(s==SpellKind::Fireball){
                Pos tgt; if(target_tile(g,6,tgt)){ cast_fireball(g,tgt); ai_turn(g); process_statuses(g); world_tick(g); }
            }
        }
    }

}






static int price_of(const Item& it){
    switch(it.kind){
        case ItemKind::Dagger: return 8;
        case ItemKind::Sword: return 15;
        case ItemKind::ArmorLeather: return 12;
        case ItemKind::ArmorChain: return 20;
        case ItemKind::PotionHeal: case ItemKind::PotionAntidote: case ItemKind::PotionRegen: return 6;
        case ItemKind::PotionStr: return 10;
        case ItemKind::SpellbookFirebolt: case ItemKind::SpellbookHeal: case ItemKind::SpellbookBlink: case ItemKind::SpellbookIce: case ItemKind::SpellbookShield: case ItemKind::SpellbookFireball: return 25;
        case ItemKind::ScrollBlink: case ItemKind::ScrollMapping: return 10;
        case ItemKind::Key: return 5;
        case ItemKind::Bomb: return 8;
        default: return 5;
    }
}
static bool near_merchant(Game& g){
    for(auto& e: g.ents){
        if(e.type==EntityType::Merchant){
            int dr = std::abs(e.pos.r - g.player.pos.r);
            int dc = std::abs(e.pos.c - g.player.pos.c);
            if(dr+dc <= 1) return true;
            if(e.pos == g.player.pos) return true;
        }
    }
    return false;
}
static void trade_modal(Game& g){
    if(!near_merchant(g)){ g.log.add("No merchant nearby."); return; }
    while(true){
        io::move(0,0); io::clear();
        std::cout<<"Merchant (Gold "<<g.gold<<")\n";
        for(size_t i=0;i<g.inv.items.size();++i){
            const auto& it=g.inv.items[i];
            std::cout<<"  "<<(char)('a'+i)<<") "<<it.name<<" - sell "<<price_of(it)<<"g\n";
        }
        if(g.inv.items.empty()) std::cout<<"  (nothing to sell)\n";
        std::cout<<"(letter) sell, (q)uit\n> "<<std::flush;
        int ch = io::getch_blocking();
        if(ch=='q') break;
        int idx = ch - 'a';
        if(idx>=0 && idx<(int)g.inv.items.size()){
            // cannot sell equipped weapon/armor
            if(idx==g.inv.weapon_idx || idx==g.inv.armor_idx){
                g.log.add("Unequip first."); continue;
            }
            int gold = price_of(g.inv.items[idx]);
            g.gold += gold;
            g.log.add("Sold for "+std::to_string(gold)+"g.");
            g.inv.items.erase(g.inv.items.begin()+idx);
            if(g.inv.weapon_idx>idx) g.inv.weapon_idx--;
            if(g.inv.armor_idx>idx) g.inv.armor_idx--;
        }
    }
}

static void world_tick(Game& g){
    // burning zones
    for(size_t i=0;i<g.firezones.size();){
        Pos z=g.firezones[i];
        if(g.player.pos==z){ g.player.mob.st.burning += 1; }
        for(auto& e: g.ents) if(e.type==EntityType::Mob && e.mob.alive && e.pos==z){ e.mob.st.burning += 1; }
        g.firettl[i]--;
        if(g.firettl[i]<=0){ g.firezones.erase(g.firezones.begin()+i); g.firettl.erase(g.firettl.begin()+i); }
        else ++i;
    }
    // bombs: tick fuse and explode when zero (3x3 square => Chebyshev radius 1)
    for(size_t i=0;i<g.ents.size();){
        auto& e = g.ents[i];
        if(e.type==EntityType::BombPlaced){
            e.fuse--;
            if(e.fuse<=0){
                int r0=e.pos.r, c0=e.pos.c;
                for(int dr=-1; dr<=1; ++dr){
                    for(int dc=-1; dc<=1; ++dc){
                        int rr=r0+dr, cc=c0+dc;
                        if(g.map.in(rr,cc)){
                            explode_at(g, rr, cc, 0); // use radius 0 cell-by-cell to reuse explosion effect
                        }
                    }
                }
                // remove bomb entity
                g.ents.erase(g.ents.begin()+i);
                continue;
            }
        }
        ++i;
    }
}
int main(){
    io::enableVT();
#ifndef _WIN32
    io::TermiosGuard tg; tg.enableRaw();
#endif
    io::hideCursor();
    Game g(24,80);
    new_game(g);
    while(g.running){
        compute_fov(g.map,g.player.pos.r,g.player.pos.c,10);
        render(g);
        Cmd cmd = read_cmd();
        switch(cmd.type){
            case CmdType::Move: move_or_attack(g,cmd.dr,cmd.dc); break;
            case CmdType::Wait: g.log.add("You wait."); break;
            case CmdType::Pickup: pickup(g); break;
            case CmdType::Inventory: inventory_modal(g); break;
            case CmdType::Search: search(g); break;
            case CmdType::Open: try_open_adjacent(g); break;
            case CmdType::Cast: spell_menu(g); break;
            case CmdType::Map: map_modal(g); break;
            case CmdType::Codex: codex_modal(g); break;
            case CmdType::Char: character_modal(g); break;
            case CmdType::Options: options_modal(g); break;
            case CmdType::Trade: trade_modal(g); break;
            case CmdType::CamPan: g.cam_follow=false; g.cam_r += cmd.dr; g.cam_c += cmd.dc; if(g.cam_r<0) g.cam_r=0; if(g.cam_c<0) g.cam_c=0; break;
            case CmdType::Descend: { auto t=g.map.at(g.player.pos.r,g.player.pos.c).t; if(t==Tile::StairsDown || t==Tile::Teleporter) next_level(g);
 else g.log.add("No exit here.");
 } break;
            case CmdType::Help: show_help(); break;
            case CmdType::SaveQuit: save_game(g); g.running=false; break;
            case CmdType::NewGame: new_game(g); break;
            case CmdType::LoadGame: if(!load_game(g)) g.log.add("No save found."); break;
            default: break;
        }
        if(cmd.type==CmdType::Move || cmd.type==CmdType::Wait || cmd.type==CmdType::Pickup || cmd.type==CmdType::Search || cmd.type==CmdType::Open || cmd.type==CmdType::Descend){
            ai_turn(g);
            process_statuses(g);
            world_tick(g);
        }
        if(g.player.mob.st.hp<=0){
            g.log.add("You die. Game over.");
            render(g);
            io::showCursor();
            std::cout<<"\n";
            return 0;
        }
    }
    io::showCursor();
    std::cout<<"\n";
    return 0;
}
