/*
 Sierra Chess — self-contained C++17 engine and local app server.
 macOS/Linux: g++ -std=c++17 -O3 chessapp.cpp -o chessapp
 Windows/MinGW: g++ -std=c++17 -O3 chessapp.cpp -o chessapp.exe -lws2_32
 Run: ./chessapp, then open http://127.0.0.1:8765.
 Keep index.html beside the executable. Ctrl+C stops the server.
 Tests: ./chessapp --test
 Count: ./chessapp --perft 4 ["FEN"]
 Think: ./chessapp --search 3200 ["FEN"]

 The HTML controls the three screens; C++ supplies rules and move search.
 Board indices: a1=0, h1=7, a8=56. Positive pieces are White.
 Piece codes: pawn=1, knight=2, bishop=3, rook=4, queen=5, king=6.
 Search target: 2800 ms; cooperative hard deadline: 3200 ms.
 Easy caps depth at 2; Medium at 4; Hard uses the available time.
 OS scheduling can delay return. This engine is educational and unrated.
 UndoState copies the position so every search move can be safely reversed.
 TT entries include history context for repetition-sensitive reuse.
 Draw claims are optional; search treats a claim as a zero-score option.
 No external libraries, Python, network service, or ML model required.
*/
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <map>
#include <csignal>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
#endif
using namespace std;

// Shared data types: colors, game outcomes, moves, and board state.
enum Color { BLACK=-1, WHITE=1 };
enum GameStatus { PLAYING, CHECKMATE, STALEMATE, INSUFFICIENT,
                  FIVEFOLD, SEVENTY_FIVE_MOVE };
// One move records its endpoints and any special move behavior.
struct Move {
    int from=0, to=0, promotion=0;
    bool enPassant=false, castle=false;
    bool operator==(const Move& m) const {
        return from==m.from && to==m.to && promotion==m.promotion
            && enPassant==m.enPassant && castle==m.castle;
    }
};
// The board and rule metadata together describe a position.
struct Position {
    array<int,64> board{};
    Color turn=WHITE;
    int castling=0; // White king/queen side=1/2; Black=4/8.
    int ep=-1, halfmove=0, fullmove=1;
    uint64_t key=0;
};
// A saved position lets search safely try a move and reverse it.
using UndoState=Position;
using GameHistory=vector<uint64_t>;
// Cached search results keep depth, score bounds, and a suggested move.
struct TTEntry {
    uint64_t key=0;
    int depth=-1, score=0, bound=0; // 0 exact, 1 lower, 2 upper.
    optional<Move> best;
};
// The caller receives a move or draw claim plus search statistics.
struct SearchResult {
    optional<Move> best;
    int score=0, depth=0;
    bool complete=false;
    uint64_t nodes=0;
    double elapsedMs=0;
    bool claimDraw=false;
};
// One search shares its clock, history, cache, and safety limits.
struct SearchContext {
    using Clock=chrono::steady_clock;
    Clock::time_point start, softDeadline, hardDeadline;
    GameHistory history;
    vector<TTEntry> table=vector<TTEntry>(1<<18);
    uint64_t nodes=0;
    int maxPly=96;
    int maxDepth=95; // Difficulty limits completed full-search depth, not tactical extensions.
};
struct SearchTimeout {};
// Scoring constants: mate scores dominate ordinary piece advantages.
constexpr int INF=1000000, MATE=100000, MATE_THRESHOLD=99000;
const string START_FEN="rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const array<int,7> VALUES={0,100,320,330,500,900,0};
// Small helpers translate piece signs and board coordinates.
int side(int piece) { return (piece>0)-(piece<0); }
bool inside(int f,int r) { return f>=0 && f<8 && r>=0 && r<8; }
string squareName(int s) { return string{char('a'+s%8),char('1'+s/8)}; }
int parseSquare(const string& s) {
    if(s.size()!=2 || s[0]<'a' || s[0]>'h' || s[1]<'1' || s[1]>'8') return -1;
    return (s[1]-'1')*8+s[0]-'a';
}
// Forward declarations allow related functions to call one another.
bool isSquareAttacked(const Position&,int,Color);
bool isInCheck(const Position&,Color);
uint64_t computePositionKey(const Position&);
vector<Move> generateLegalMoves(Position&);

// REM: Detect attacks directly, without recursively generating legal moves.
// Pinned pieces still attack squares for king movement and castling rules.
bool isSquareAttacked(const Position& p,int s,Color attacker) {
    // Split the target square into its file and rank.
    int f=s%8,r=s/8;
    // Check the two squares from which enemy pawns could attack.
    for(int df : {-1,1}) {
        int pr=r-int(attacker);
        if(inside(f+df,pr) && p.board[pr*8+f+df]==int(attacker)) return true;
    }
    // Check the eight possible knight jumps.
    const int knight[8][2]={{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    for(auto& d:knight) if(inside(f+d[0],r+d[1]) &&
        p.board[(r+d[1])*8+f+d[0]]==2*attacker) return true;
    // Scan straight and diagonal rays for sliding pieces or an adjacent king.
    const int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    for(int i=0;i<8;i++) {
        int x=f+dirs[i][0],y=r+dirs[i][1],distance=1;
        while(inside(x,y)) {
            int piece=p.board[y*8+x];
            if(piece) {
                if(side(piece)==attacker) {
                    int kind=abs(piece);
                    if(kind==5 || (i<4 && kind==4) || (i>=4 && kind==3)
                        || (distance==1 && kind==6)) return true;
                }
                // The first occupied square blocks any attack from farther along this ray.
                break;
            }
            x+=dirs[i][0]; y+=dirs[i][1]; ++distance;
        }
    }
    return false;
}

// REM: Locate the specified king and ask whether the opponent attacks it.
bool isInCheck(const Position& p,Color color) {
    // Find the king, then test attacks by the opposite color.
    for(int s=0;s<64;s++) if(p.board[s]==6*color)
        return isSquareAttacked(p,s,Color(-color));
    throw logic_error("Position has no king");
}

// REM: Hash pieces, turn, rights, and only legally usable en passant.
// Counters are excluded from repetition identity; TT adds its own draw context.
uint64_t computePositionKey(const Position& p) {
    // Start a deterministic hash and mix in the position fields.
    uint64_t h=14695981039346656037ULL;
    auto mix=[&](int v) { h^=uint64_t(v+16); h*=1099511628211ULL; };
    for(int piece:p.board) mix(piece);
    mix(p.turn); mix(p.castling);
    // Include an en passant target only if a legal capture is possible.
    int relevantEP=-1;
    if(p.ep>=0) {
        int sourceRank=p.ep/8-int(p.turn);
        int captured=p.ep-8*int(p.turn);
        for(int df:{-1,1}) {
            int f=p.ep%8+df;
            if(!inside(f,sourceRank)) continue;
            int from=sourceRank*8+f;
            if(p.board[from]!=p.turn || p.board[p.ep]!=0 ||
                p.board[captured]!=-int(p.turn)) continue;
            // Try the capture on a copy to check whether it exposes the king.
            Position test=p;
            test.board[from]=0; test.board[captured]=0; test.board[p.ep]=p.turn;
            if(!isInCheck(test,p.turn)) { relevantEP=p.ep; break; }
        }
    }
    mix(relevantEP); return h;
}

// REM: Parse and validate FEN structure and essential position invariants.
// This checks structural legality, not whether the full game history is reachable.
Position loadFEN(const string& fen) {
    // Read the six FEN fields: board, turn, castling, en passant, and counters.
    Position p;
    string board,turn,rights,ep,extra;
    istringstream in(fen);
    if(!(in>>board>>turn>>rights>>ep>>p.halfmove>>p.fullmove) || (in>>extra))
        throw invalid_argument("FEN needs exactly six fields");
    // Decode ranks from eight down to one, expanding digits into empty squares.
    int rank=7,file=0,whiteKings=0,blackKings=0;
    const string pieces=" PNBRQK";
    for(char c:board) {
        if(c=='/') {
            if(file!=8 || rank==0) throw invalid_argument("Invalid FEN rank");
            --rank; file=0;
        } else if(c>='1' && c<='8') {
            file+=c-'0'; if(file>8) throw invalid_argument("Too many squares");
        } else {
            char upper=char(toupper(static_cast<unsigned char>(c)));
            auto kind=pieces.find(upper);
            if(kind==string::npos || kind==0 || file>=8) throw invalid_argument("Invalid FEN piece");
            int value=int(kind)*(isupper(static_cast<unsigned char>(c))?1:-1);
            if(abs(value)==1 && (rank==0 || rank==7)) throw invalid_argument("Pawn on promotion rank");
            p.board[rank*8+file++]=value;
            whiteKings+=(value==6); blackKings+=(value==-6);
        }
    }
    // Reject incomplete boards and missing or duplicate kings.
    if(rank!=0 || file!=8 || whiteKings!=1 || blackKings!=1)
        throw invalid_argument("FEN requires eight ranks and one king per side");
    // Validate the player to move and decode castling-right flags.
    if(turn!="w" && turn!="b") throw invalid_argument("Invalid side to move");
    p.turn=turn=="w"?WHITE:BLACK;
    if(rights!="-") for(char c:rights) {
        int bit=c=='K'?1:c=='Q'?2:c=='k'?4:c=='q'?8:0;
        if(!bit || (p.castling&bit)) throw invalid_argument("Invalid castling rights");
        p.castling|=bit;
    }
    // Castling rights require the corresponding king and rook on their home squares.
    if(((p.castling&3) && p.board[4]!=6) || ((p.castling&12) && p.board[60]!=-6)
        || ((p.castling&1) && p.board[7]!=4) || ((p.castling&2) && p.board[0]!=4)
        || ((p.castling&4) && p.board[63]!=-4) || ((p.castling&8) && p.board[56]!=-4))
        throw invalid_argument("Castling rights do not match pieces");
    // Check that an en passant target matches the last double pawn advance.
    if(ep!="-") {
        p.ep=parseSquare(ep);
        if(p.ep<0 || p.ep/8!=(p.turn==WHITE?5:2) || p.board[p.ep]!=0
            || p.board[p.ep-8*p.turn]!=-int(p.turn) || p.board[p.ep+8*p.turn]!=0)
            throw invalid_argument("Invalid en passant target");
    }
    // Validate counters and king safety before calculating the position key.
    if(p.halfmove<0 || p.fullmove<1) throw invalid_argument("Invalid move counters");
    if(isInCheck(p,Color(-p.turn))) throw invalid_argument("Side that just moved is in check");
    p.key=computePositionKey(p); return p;
}

// REM: Construct the standard initial chess position.
Position createStartingPosition() { return loadFEN(START_FEN); }

// REM: Show ranks eight through one, with uppercase White pieces.
void printBoard(const Position& p) {
    const string names=".PNBRQK";
    // Print each rank, using lowercase for Black and dots for empty squares.
    for(int r=7;r>=0;r--) {
        cout<<r+1<<"  ";
        for(int f=0;f<8;f++) {
            int piece=p.board[r*8+f]; char c=names[abs(piece)];
            if(piece<0) c=char(tolower(static_cast<unsigned char>(c)));
            cout<<c<<' ';
        }
        cout<<'\n';
    }
    // Finish with file labels and the current turn.
    cout<<"   a b c d e f g h\n"<<(p.turn==WHITE?"White":"Black")<<" to move\n";
}

// REM: Generate piece-rule candidates, including special moves.
// King safety for ordinary moves is checked by generateLegalMoves.
vector<Move> generatePseudoLegalMoves(const Position& p) {
    vector<Move> moves; moves.reserve(64);
    int color=p.turn;
    // A pawn reaching the last rank creates four distinct promotion choices.
    auto add=[&](int from,int to,bool ep=false,bool castle=false) {
        if(abs(p.board[from])==1 && (to/8==0 || to/8==7))
            for(int kind:{5,4,3,2}) moves.push_back({from,to,kind,ep,castle});
        else moves.push_back({from,to,0,ep,castle});
    };
    // Direction tables describe sliding rays and knight jumps.
    const int dirs[8][2]={{1,0},{-1,0},{0,1},{0,-1},{1,1},{1,-1},{-1,1},{-1,-1}};
    const int knights[8][2]={{1,2},{2,1},{2,-1},{1,-2},{-1,-2},{-2,-1},{-2,1},{-1,2}};
    // Visit only pieces belonging to the player whose turn it is.
    for(int s=0;s<64;s++) {
        if(side(p.board[s])!=color) continue;
        int f=s%8,r=s/8,kind=abs(p.board[s]);
        // Pawn moves: forward advances, diagonal captures, and en passant.
        if(kind==1) {
            int nr=r+color;
            if(inside(f,nr) && p.board[nr*8+f]==0) {
                add(s,nr*8+f);
                if(r==(color==WHITE?1:6) && p.board[(r+2*color)*8+f]==0)
                    add(s,(r+2*color)*8+f);
            }
            // Pawn captures use diagonals; forward movement cannot capture.
            for(int df:{-1,1}) if(inside(f+df,nr)) {
                int to=nr*8+f+df,target=p.board[to];
                if(side(target)==-color && abs(target)!=6) add(s,to);
                if(to==p.ep && !target && p.board[to-8*color]==-color) add(s,to,true);
            }
        // Knights jump directly to their destination and ignore intervening pieces.
        } else if(kind==2) {
            for(auto& d:knights) if(inside(f+d[0],r+d[1])) {
                int to=(r+d[1])*8+f+d[0];
                if(side(p.board[to])!=color && abs(p.board[to])!=6) add(s,to);
            }
        // Bishops, rooks, and queens slide; kings use the same directions for one step.
        } else {
            for(int i=0;i<8;i++) {
                if((kind==3 && i<4) || (kind==4 && i>=4)) continue;
                int x=f+dirs[i][0],y=r+dirs[i][1];
                while(inside(x,y)) {
                    int to=y*8+x,target=p.board[to];
                    if(side(target)==color) break;
                    if(abs(target)!=6) add(s,to);
                    if(target || kind==6) break;
                    x+=dirs[i][0]; y+=dirs[i][1];
                }
            }
            // Castling additionally requires rights, clear paths, and safe king squares.
            if(kind==6) {
                int home=color==WHITE?4:60,ks=color==WHITE?1:4,qs=color==WHITE?2:8;
                Color enemy=Color(-color);
                if(s==home && !isInCheck(p,p.turn)) {
                    if((p.castling&ks) && p.board[home+3]==4*color
                        && !p.board[home+1] && !p.board[home+2]
                        && !isSquareAttacked(p,home+1,enemy) && !isSquareAttacked(p,home+2,enemy))
                        add(s,home+2,false,true);
                    if((p.castling&qs) && p.board[home-4]==4*color
                        && !p.board[home-1] && !p.board[home-2] && !p.board[home-3]
                        && !isSquareAttacked(p,home-1,enemy) && !isSquareAttacked(p,home-2,enemy))
                        add(s,home-2,false,true);
                }
            }
        }
    }
    return moves;
}

// REM: Apply a generated move; return a full snapshot for exact reversal.
// This low-level function assumes the caller has validated the move.
UndoState makeMove(Position& p,const Move& m) {
    // Save the original position before changing any fields.
    UndoState undo=p;
    int piece=p.board[m.from],captured=p.board[m.to],color=p.turn;
    // Move the piece, remove an en passant victim, and apply promotion.
    p.board[m.from]=0;
    if(m.enPassant) { captured=p.board[m.to-8*color]; p.board[m.to-8*color]=0; }
    p.board[m.to]=m.promotion?m.promotion*color:piece;
    // During castling, move the rook as well as the king.
    if(m.castle) {
        int rookFrom=m.to>m.from?m.from+3:m.from-4;
        int rookTo=m.to>m.from?m.from+1:m.from-1;
        p.board[rookTo]=p.board[rookFrom]; p.board[rookFrom]=0;
    }
    // King moves and moves involving rook home squares remove castling rights.
    if(piece==6) p.castling&=~3;
    if(piece==-6) p.castling&=~12;
    auto clearRookRight=[&](int s) {
        if(s==0) p.castling&=~2;
        if(s==7) p.castling&=~1;
        if(s==56) p.castling&=~8;
        if(s==63) p.castling&=~4;
    };
    clearRookRight(m.from); clearRookRight(m.to);
    // Update en passant and draw counters, switch turns, and refresh the hash.
    p.ep=abs(piece)==1 && abs(m.to-m.from)==16?(m.from+m.to)/2:-1;
    p.halfmove=(abs(piece)==1 || captured)?0:p.halfmove+1;
    if(color==BLACK) ++p.fullmove;
    p.turn=Color(-color); p.key=computePositionKey(p);
    return undo;
}

// REM: Restore all board and metadata fields, including the position key.
void undoMove(Position& p,const Move&,const UndoState& undo) { p=undo; }

// REM: Reject any candidate that leaves the moving side's king attacked.
vector<Move> generateLegalMoves(Position& p) {
    vector<Move> legal;
    Color mover=p.turn;
    // Temporarily apply each candidate and test the moving king.
    for(const Move& m:generatePseudoLegalMoves(p)) {
        auto undo=makeMove(p,m);
        bool safe=!isInCheck(p,mover);
        // Restore the board before keeping or rejecting the candidate.
        undoMove(p,m,undo);
        if(safe) legal.push_back(m);
    }
    return legal;
}

// REM: Format a move as coordinate notation; append promotion if needed.
string moveToString(const Move& m) {
    // Combine source and destination, then append a promotion letter if present.
    string text=squareName(m.from)+squareName(m.to);
    if(m.promotion) text+=string(" pnbrqk")[m.promotion];
    return text;
}

// REM: Accept only coordinates matching an actual legal move.
optional<Move> parseMove(const Position& position,const string& input) {
    // Use a copy because legal-move generation temporarily changes the board.
    Position p=position;
    for(const Move& m:generateLegalMoves(p)) if(moveToString(m)==input) return m;
    return nullopt;
}

// Count occurrences of this position, including the current position.
int repetitionCount(const Position& p,const GameHistory& history) {
    return int(count(history.begin(),history.end(),p.key));
}
// Recognize common positions where neither side has mating material.
bool insufficientMaterial(const Position& p) {
    int minors=0,bishops=0,bishopColor=-1;
    bool sameColor=true;
    // Count minor pieces; pawns, rooks, or queens rule out this simple draw test.
    for(int s=0;s<64;s++) {
        int kind=abs(p.board[s]);
        if(kind==0 || kind==6) continue;
        if(kind==1 || kind==4 || kind==5) return false;
        ++minors;
        if(kind==3) {
            ++bishops; int color=(s%8+s/8)%2;
            if(bishopColor>=0 && color!=bishopColor) sameColor=false;
            bishopColor=color;
        }
    }
    // Recognize bare kings, one minor piece, or bishops all on the same square color.
    return minors<=1 || (minors==bishops && sameColor);
}
// A halfmove is one player move; 100 halfmoves equal 50 full moves.
bool canClaimDraw(const Position& p,const GameHistory& history) {
    return p.halfmove>=100 || repetitionCount(p,history)>=3;
}

// REM: Check mate/stalemate first, then safe material and automatic draws.
// Recognizes standard insufficient-material cases, not every possible dead position.
GameStatus getGameStatus(Position& p,const GameHistory& history) {
    // No legal moves means either checkmate or stalemate; check this first.
    if(generateLegalMoves(p).empty()) return isInCheck(p,p.turn)?CHECKMATE:STALEMATE;
    // Then check material draws and automatic repetition/move-count draws.
    if(insufficientMaterial(p)) return INSUFFICIENT;
    if(p.halfmove>=150) return SEVENTY_FIVE_MOVE;
    if(repetitionCount(p,history)>=5) return FIVEFOLD;
    return PLAYING;
}

// REM: Sum White's material minus Black's material in centipawns.
int evaluateMaterial(const Position& p) {
    int score=0;
    // Multiply each piece value by its color sign to compare the two armies.
    for(int piece:p.board) score+=side(piece)*VALUES[abs(piece)];
    return score;
}

// REM: Estimate activity, advancement, pawn structure, and king shelter.
// Blend king placement by remaining material to encourage endgame activity.
int evaluatePosition(const Position& p) {
    int score=0,phase=0;
    // Count pawns by file and estimate the game phase from remaining pieces.
    int pawns[2][8]{};
    // Count the material that distinguishes middlegame from endgame.
    for(int s=0;s<64;s++) {
        int piece=p.board[s],kind=abs(piece);
        // Reward pawn advancement; penalize doubled or isolated pawns.
        if(kind==1) ++pawns[piece>0?0:1][s%8];
        if(kind==2 || kind==3) ++phase;
        if(kind==4) phase+=2;
        if(kind==5) phase+=4;
    }
    // Cap the phase at the normal starting-material level.
    phase=min(24,phase);
    for(int s=0;s<64;s++) {
        int piece=p.board[s]; if(!piece) continue;
        int color=side(piece),kind=abs(piece),f=s%8,r=s/8;
        // Measure forward progress relative to color and closeness to the center.
        int advance=color==WHITE?r:7-r;
        int center=14-(abs(2*f-7)+abs(2*r-7));
        int bonus=0,index=color==WHITE?0:1;
        if(kind==1) {
            bonus=advance*7+center*2;
            if(pawns[index][f]>1) bonus-=12;
            if((f==0 || !pawns[index][f-1]) && (f==7 || !pawns[index][f+1])) bonus-=12;
            // A passed pawn has no opposing pawn ahead on its file or neighboring files.
            bool passed=true;
            for(int rr=r+color;rr>=0 && rr<8;rr+=color)
                for(int ff=max(0,f-1);ff<=min(7,f+1);ff++)
                    if(p.board[rr*8+ff]==-color) passed=false;
            if(passed) bonus+=advance*advance*3;
        }
        // Reward centralized minor pieces and active rooks and queens.
        if(kind==2) bonus=center*5;
        if(kind==3) bonus=center*3;
        if(kind==4) {
            bonus=advance*2;
            if(!pawns[index][f]) bonus+=10;
            if(!pawns[0][f] && !pawns[1][f]) bonus+=8;
        }
        if(kind==5) bonus=center;
        // Favor a sheltered king early and a centralized king in the endgame.
        if(kind==6) {
            int middle=-center*3-advance*8;
            if(advance==0 && (f==2 || f==6)) middle+=30;
            int shieldRank=r+color;
            for(int ff=max(0,f-1);ff<=min(7,f+1);ff++)
                if(inside(ff,shieldRank) && p.board[shieldRank*8+ff]==color) middle+=10;
            // Blend the two king scores according to the remaining material.
            bonus=(middle*phase+center*5*(24-phase))/24;
        }
        score+=color*bonus;
    }
    // Mobility is pseudo-legal activity, deliberately cheap to calculate.
    Position white=p,black=p; white.turn=WHITE; black.turn=BLACK;
    white.ep=black.ep=-1;
    score+=2*(int(generatePseudoLegalMoves(white).size())-int(generatePseudoLegalMoves(black).size()));
    return score;
}

// REM: Return the estimated advantage for the player whose turn it is.
int evaluate(const Position& p) {
    return p.turn*(evaluateMaterial(p)+evaluatePosition(p));
}

// Combine board identity with counters and history for draw-sensitive caching.
uint64_t tableKey(const Position& p,const SearchContext& c) {
    uint64_t key=p.key ^ (uint64_t(p.halfmove)*0x9e3779b97f4a7c15ULL);
    // Preserve reversible-history order to avoid unsafe draw-context reuse.
    size_t begin=c.history.size()>size_t(p.halfmove+1)?c.history.size()-p.halfmove-1:0;
    for(size_t i=begin;i<c.history.size();i++)
        key^=c.history[i]+0x9e3779b97f4a7c15ULL+(key<<6)+(key>>2);
    return key;
}

// REM: Retrieve an entry only when its full context key matches.
optional<TTEntry> probeTranspositionTable(const Position& p,const SearchContext& c) {
    uint64_t key=tableKey(p,c);
    // Map the context key to a slot; matching the full key rejects collisions.
    const auto& e=c.table[key%c.table.size()];
    if(e.depth>=0 && e.key==key) return e;
    return nullopt;
}

// REM: Store a bounded-size table entry; mate normalization is done by caller.
void storeTranspositionTable(const Position& p,const TTEntry& entry,SearchContext& c) {
    uint64_t key=tableKey(p,c);
    // Copy the result into its fixed-size cache slot.
    TTEntry stored=entry; stored.key=key;
    c.table[key%c.table.size()]=stored;
}

// REM: Put the previous best move first, then promotions and valuable captures.
void orderMoves(const Position& p,vector<Move>& moves,
                const optional<Move>& preferred,const SearchContext&) {
    // Give each move a priority score so promising moves are searched first.
    auto score=[&](const Move& m) {
        if(preferred && m==*preferred) return 1000000;
        // Favor valuable victims, cheaper attackers, and promotions.
        int victim=m.enPassant?1:abs(p.board[m.to]);
        return (victim?10000+10*VALUES[victim]-VALUES[abs(p.board[m.from])]:0)
            +(m.promotion?8000+VALUES[m.promotion]:0);
    };
    // Sort by descending priority while preserving order for ties.
    stable_sort(moves.begin(),moves.end(),[&](const Move& a,const Move& b) { return score(a)>score(b); });
}

// REM: Cancel search at the hard deadline using a steady, monotonic clock.
void checkDeadline(SearchContext& c) {
    if(SearchContext::Clock::now()>=c.hardDeadline) throw SearchTimeout{};
}

// REM: Always undo a search move and remove its history entry, even on timeout.
template<class Function>
int searchChild(Position& p,const Move& m,SearchContext& c,Function fn,
                bool* claimedByIntendedMove=nullptr) {
    // Apply the trial move and add its resulting position to search history.
    auto undo=makeMove(p,m);
    c.history.push_back(p.key);
    try {
        // The mover may claim a draw by announcing this intended move.
        bool eligible=canClaimDraw(p,c.history);
        // Run the supplied recursive search and account for an optional draw claim.
        int score=fn();
        if(claimedByIntendedMove) *claimedByIntendedMove=eligible && score<=0;
        if(eligible) score=max(score,0);
        // Undo the trial move before returning its score.
        c.history.pop_back(); undoMove(p,m,undo); return score;
    // Restore the board even when a timeout or another exception interrupts search.
    } catch(...) {
        c.history.pop_back(); undoMove(p,m,undo); throw;
    }
}
// Store mate distance relative to this position rather than the current search root.
int packMate(int score,int ply) {
    return score>MATE_THRESHOLD?score+ply:score<-MATE_THRESHOLD?score-ply:score;
}
// Restore a cached mate score to the current root-relative ply distance.
int unpackMate(int score,int ply) {
    return score>MATE_THRESHOLD?score-ply:score<-MATE_THRESHOLD?score+ply:score;
}

// REM: Extend captures/promotions until relatively quiet; search all check evasions.
// Detect terminal states before static scoring and honor the same hard deadline.
int quiescence(Position& p,int alpha,int beta,int ply,SearchContext& c) {
    // Check time and detect terminal states before extending tactical play.
    checkDeadline(c); ++c.nodes;
    bool check=isInCheck(p,p.turn);
    auto moves=generateLegalMoves(p);
    if(moves.empty()) return check?-MATE+ply:0;
    if(insufficientMaterial(p) || p.halfmove>=150 || repetitionCount(p,c.history)>=5) return 0;
    // Treat an available draw claim as a zero-score option.
    bool claim=canClaimDraw(p,c.history);
    if(claim) { if(0>=beta) return 0; alpha=max(alpha,0); }
    // Stop extreme extensions at the search-ply safety limit.
    if(ply>=c.maxPly) return max(claim?0:-INF,evaluate(p));
    // Only a king out of check may use the static position as a stopping estimate.
    if(!check) {
        int stand=evaluate(p);
        if(stand>=beta) return stand;
        alpha=max(alpha,stand);
        // In quiet positions, extend only captures and promotions.
        moves.erase(remove_if(moves.begin(),moves.end(),[&](const Move& m) {
            return !p.board[m.to] && !m.enPassant && !m.promotion;
        }),moves.end());
    }
    // Search tactical moves first; when in check, keep every legal escape.
    orderMoves(p,moves,nullopt,c);
    for(const auto& m:moves) {
        int score=searchChild(p,m,c,[&] { return -quiescence(p,-beta,-alpha,ply+1,c); });
        // Stop this branch when its score already meets the upper search bound.
        if(score>=beta) return score;
        alpha=max(alpha,score);
    }
    return alpha;
}

// REM: Negamax alpha-beta searches alternating sides, with context-safe caching.
// Return centipawns for the side to move; a forced loss approaches -MATE.
int alphaBeta(Position& p,int depth,int alpha,int beta,int ply,SearchContext& c) {
    checkDeadline(c);
    // At the normal depth limit, resolve immediate tactics with quiescence.
    if(depth<=0) return quiescence(p,alpha,beta,ply,c);
    ++c.nodes;
    // Detect mate, stalemate, and automatic draws before ordinary evaluation.
    auto moves=generateLegalMoves(p);
    if(moves.empty()) return isInCheck(p,p.turn)?-MATE+ply:0;
    if(insufficientMaterial(p) || p.halfmove>=150 || repetitionCount(p,c.history)>=5) return 0;
    bool claim=canClaimDraw(p,c.history);
    if(ply>=c.maxPly) return max(claim?0:-INF,evaluate(p));
    // Remember the original bound and include a draw claim when available.
    int originalAlpha=alpha,bestScore=claim?0:-INF;
    optional<Move> best;
    if(claim) { alpha=max(alpha,0); if(alpha>=beta) return 0; }
    // Reuse a sufficiently deep cached result if its bound answers this search.
    auto cached=probeTranspositionTable(p,c);
    if(cached && cached->depth>=depth) {
        int score=unpackMate(cached->score,ply);
        if(cached->bound==0 || (cached->bound==1 && score>=beta)
            || (cached->bound==2 && score<=alpha)) return score;
    }
    // Search the cached best move first to make pruning more effective.
    orderMoves(p,moves,cached?cached->best:nullopt,c);
    for(const auto& m:moves) {
        // Negate the opponent score and swap bounds because the side to move changes.
        int score=searchChild(p,m,c,[&] { return -alphaBeta(p,depth-1,-beta,-alpha,ply+1,c); });
        if(score>bestScore) { bestScore=score; best=m; }
        alpha=max(alpha,score);
        // Prune remaining moves once the opponent would avoid this branch.
        if(alpha>=beta) break;
    }
    // Label the result exact, lower-bound, or upper-bound before caching it.
    int bound=bestScore<=originalAlpha?2:bestScore>=beta?1:0;
    storeTranspositionTable(p,{0,depth,packMate(bestScore,ply),bound,best},c);
    return bestScore;
}

// REM: Search one complete root depth; never publish a partially searched result.
SearchResult searchRoot(Position& p,int depth,SearchContext& c) {
    checkDeadline(c);
    // Prepare the actual candidate moves for this one search depth.
    auto moves=generateLegalMoves(p);
    SearchResult result; result.depth=depth;
    if(moves.empty()) { result.score=isInCheck(p,p.turn)?-MATE:0; result.complete=true; return result; }
    // A current draw claim provides a baseline that a winning move can improve.
    bool claim=canClaimDraw(p,c.history);
    int alpha=claim?0:-INF;
    result.score=alpha; result.claimDraw=claim;
    auto cached=probeTranspositionTable(p,c);
    orderMoves(p,moves,cached?cached->best:nullopt,c);
    // Compare root moves and keep any draw claim based on an intended move.
    for(const auto& m:moves) {
        bool intendedClaim=false;
        int score=searchChild(p,m,c,[&] { return -alphaBeta(p,depth-1,-INF,-alpha,1,c); },&intendedClaim);
        if(score>result.score) { result.score=score; result.best=m; result.claimDraw=intendedClaim; }
        alpha=max(alpha,score);
    }
    // Publish the result only after all required root work finishes.
    checkDeadline(c);
    result.complete=true;
    storeTranspositionTable(p,{0,depth,packMate(result.score,0),0,result.best},c);
    return result;
}

// REM: Iteratively deepen until the soft target or hard cancellation.
// Preserve a legal fallback and only replace it after a completed depth.
SearchResult chooseBestMove(Position& p,const GameHistory& history,SearchContext& c) {
    SearchResult best;
    // Initialize search statistics and make sure history includes the current board.
    c.nodes=0; c.history=history;
    if(c.history.empty() || c.history.back()!=p.key) c.history.push_back(p.key);
    auto moves=generateLegalMoves(p);
    if(getGameStatus(p,c.history)==PLAYING) {
        // Keep a legal fallback in case the clock expires before depth one finishes.
        best.best=moves.front();
        if(canClaimDraw(p,c.history)) { best.claimDraw=true; best.best.reset(); }
        try {
            // Try increasingly deep searches until time runs out or a mate is found.
            for(int depth=1;depth<c.maxPly && depth<=c.maxDepth;depth++) {
                // The soft target prevents starting another full iteration too late.
                if(SearchContext::Clock::now()>=c.softDeadline) break;
                auto result=searchRoot(p,depth,c);
                // Only a completed iteration may replace the saved answer.
                if(result.complete) best=result;
                if(abs(best.score)>MATE_THRESHOLD) break;
            }
        } catch(const SearchTimeout&) { /* Last complete result remains available. */ }
    }
    // Attach total work and elapsed wall-clock time to the chosen result.
    best.nodes=c.nodes;
    best.elapsedMs=chrono::duration<double,milli>(SearchContext::Clock::now()-c.start).count();
    return best;
}

// The soft deadline stops new iterations; the hard deadline cancels active search.
void setBudget(SearchContext& c,int softMs,int hardMs) {
    c.start=SearchContext::Clock::now();
    c.softDeadline=c.start+chrono::milliseconds(softMs);
    c.hardDeadline=c.start+chrono::milliseconds(hardMs);
}
// Translate a game outcome into a readable terminal message.
string statusText(GameStatus s) {
    switch(s) {
        case CHECKMATE: return "Checkmate";
        case STALEMATE: return "Draw: stalemate";
        case INSUFFICIENT: return "Draw: insufficient mating material";
        case FIVEFOLD: return "Draw: fivefold repetition";
        case SEVENTY_FIVE_MOVE: return "Draw: 75-move rule";
        default: return "Game in progress";
    }
}

// REM: Maintain the actual game history and alternate human and timed bot moves.
// Humans can claim eligible draws now or by specifying an intended legal move.
void playGame() {
    // Set up the board, permanent game history, and reusable search cache.
    Position p=createStartingPosition(); GameHistory history{p.key}; SearchContext c;
    // Let the human choose a color and show the accepted commands.
    cout<<"Cesar's Chess Bot\nPlay White or Black? [w/b]: ";
    string input; if(!getline(cin,input)) return;
    Color human=input=="b"?BLACK:WHITE;
    cout<<"Moves: e2e4; promotion: e7e8q. Commands: moves, claim, claim <move>, quit.\n";
    // Each turn starts by displaying the board and checking whether play has ended.
    while(true) {
        printBoard(p);
        auto status=getGameStatus(p,history);
        if(status!=PLAYING) {
            cout<<statusText(status);
            if(status==CHECKMATE) cout<<" — "<<(p.turn==WHITE?"Black":"White")<<" wins";
            cout<<'\n'; break;
        }
        if(isInCheck(p,p.turn)) cout<<"Check!\n";
        // Human turn: process commands or validate a coordinate move.
        if(p.turn==human) {
            cout<<"> "; if(!getline(cin,input) || input=="quit") break;
            if(input=="moves") {
                for(auto m:generateLegalMoves(p)) cout<<moveToString(m)<<' ';
                cout<<'\n'; continue;
            }
            // Check a draw claim now or simulate an announced intended move.
            if(input=="claim" || input.rfind("claim ",0)==0) {
                bool eligible=canClaimDraw(p,history);
                if(!eligible && input.rfind("claim ",0)==0) {
                    auto intended=parseMove(p,input.substr(6));
                    if(intended) {
                        auto undo=makeMove(p,*intended); history.push_back(p.key);
                        eligible=canClaimDraw(p,history);
                        history.pop_back(); undoMove(p,*intended,undo);
                    }
                }
                if(eligible) { cout<<"Draw claimed.\n"; break; }
                cout<<"A draw cannot be claimed with that position/move.\n"; continue;
            }
            // Apply a valid human move and remember the resulting position.
            auto move=parseMove(p,input);
            if(!move) { cout<<"Invalid or illegal move. Try e2e4.\n"; continue; }
            makeMove(p,*move); history.push_back(p.key);
        } else {
            // Bot turn: set the time budget, search, then report and apply the decision.
            cout<<"Bot is thinking...\n";
            setBudget(c,2800,3200);
            auto result=chooseBestMove(p,history,c);
            if(result.claimDraw) {
                cout<<"Bot claims a draw";
                if(result.best) cout<<" by intended move "<<moveToString(*result.best);
                cout<<".\n"; break;
            }
            if(!result.best) throw logic_error("No bot move in a playable position");
            cout<<"Bot: "<<moveToString(*result.best)<<" | depth "<<result.depth
                <<" | score "<<result.score<<" cp | "<<result.elapsedMs<<" ms\n";
            makeMove(p,*result.best); history.push_back(p.key);
        }
    }
}

// REM: Count legal move sequences; this checks move rules independently of search.
uint64_t perft(Position& p,int depth) {
    // An empty sequence counts as one; at depth one, count legal moves.
    if(depth==0) return 1;
    auto moves=generateLegalMoves(p);
    if(depth==1) return moves.size();
    // Recursively count longer legal sequences, undoing every trial move.
    uint64_t nodes=0;
    for(auto m:moves) { auto undo=makeMove(p,m); nodes+=perft(p,depth-1); undoMove(p,m,undo); }
    return nodes;
}
// Compare every position field to verify exact restoration.
bool samePosition(const Position& a,const Position& b) {
    return a.board==b.board && a.turn==b.turn && a.castling==b.castling
        && a.ep==b.ep && a.halfmove==b.halfmove && a.fullmove==b.fullmove && a.key==b.key;
}
// Report a passing check or stop testing at the first failed condition.
void require(bool ok,const string& name) {
    if(!ok) throw runtime_error("FAILED: "+name);
    cout<<"PASS: "<<name<<'\n';
}

// REM: Regression checks for move generation, special moves, draws, and timing.
void runTests() {
    // Check known move-sequence totals from the initial and special test positions.
    auto start=createStartingPosition(); auto original=start;
    const uint64_t expected[]={1,20,400,8902,197281};
    for(int d=1;d<=4;d++) require(perft(start,d)==expected[d],"starting perft depth "+to_string(d));
    require(samePosition(start,original),"perft restores board");
    auto kiwi=loadFEN("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    require(perft(kiwi,1)==48 && perft(kiwi,2)==2039 && perft(kiwi,3)==97862,"castling-rich perft");
    auto endgame=loadFEN("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    require(perft(endgame,3)==2812 && perft(endgame,4)==43238,"rook/pawn endgame perft");
    // Verify promotion choices and legal versus pinned en passant captures.
    auto promote=loadFEN("4k3/P7/8/8/8/8/8/4K3 w - - 0 1");
    int promotions=0; for(auto m:generateLegalMoves(promote)) if(m.promotion) ++promotions;
    require(promotions==4,"all four promotions");
    auto ep=loadFEN("k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    require(!parseMove(ep,"e5d6"),"en passant cannot expose own king");
    auto noEP=loadFEN("k3r3/8/8/3pP3/8/8/8/4K3 w - - 0 1");
    require(ep.key==noEP.key,"unusable en passant excluded from repetition key");
    auto legalEP=loadFEN("k7/8/8/3pP3/8/8/8/4K3 w - d6 0 1");
    auto epMove=parseMove(legalEP,"e5d6"); require(bool(epMove),"legal en passant");
    auto undo=makeMove(legalEP,*epMove);
    require(legalEP.board[35]==0 && legalEP.board[43]==1,"en passant removes captured pawn");
    undoMove(legalEP,*epMove,undo); require(samePosition(legalEP,undo),"en passant undo");
    // Check that every candidate can be applied and exactly undone.
    for(auto m:generateLegalMoves(kiwi)) {
        auto saved=kiwi; auto old=makeMove(kiwi,m); undoMove(kiwi,m,old);
        require(samePosition(kiwi,saved),"make/undo "+moveToString(m));
    }
    // Check mate, stalemate, claimable draws, and automatic draws.
    auto mate=loadFEN("7k/6Q1/5K2/8/8/8/8/8 b - - 150 1");
    require(getGameStatus(mate,{mate.key})==CHECKMATE,"mate precedes 75-move draw");
    auto stale=loadFEN("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    require(getGameStatus(stale,{stale.key})==STALEMATE,"stalemate");
    require(canClaimDraw(start,{start.key,start.key,start.key}),"threefold claim");
    require(getGameStatus(start,GameHistory(5,start.key))==FIVEFOLD,"fivefold automatic draw");
    auto fifty=loadFEN("7k/8/8/8/8/8/8/KR6 w - - 100 51");
    require(canClaimDraw(fifty,{fifty.key}) && getGameStatus(fifty,{fifty.key})==PLAYING,
        "50-move draw is claimable, not automatic");
    fifty.halfmove=150;
    require(getGameStatus(fifty,{fifty.key})==SEVENTY_FIVE_MOVE,"75-move automatic draw");
    auto material=loadFEN("7k/8/8/8/8/8/8/K7 w - - 0 1");
    require(getGameStatus(material,{material.key})==INSUFFICIENT,"bare kings draw");
    // Verify mate detection by search and safe cancellation under short budgets.
    auto tactic=loadFEN("7k/8/5KQ1/8/8/8/8/8 w - - 0 1");
    SearchContext c; setBudget(c,150,200);
    auto result=chooseBestMove(tactic,{tactic.key},c);
    require(bool(result.best),"mate search supplies move");
    makeMove(tactic,*result.best);
    require(getGameStatus(tactic,{tactic.key})==CHECKMATE,"finds mate in one");
    setBudget(c,40,60); original=start;
    result=chooseBestMove(start,{start.key},c);
    require(result.best && parseMove(start,moveToString(*result.best)),"timed search returns legal move");
    require(samePosition(start,original) && c.history==GameHistory{start.key},"timeout restores position and history");
    require(result.elapsedMs<500,"short-budget search stops promptly");
    // Check a draw claim by intended move and an immediate-timeout fallback.
    auto losing=loadFEN("7k/8/8/8/4q3/8/8/K7 w - - 99 50");
    setBudget(c,40,60);
    result=chooseBestMove(losing,{losing.key},c);
    require(result.claimDraw && result.best,"claims 50-move draw by intended move");
    setBudget(c,0,0); result=chooseBestMove(start,{start.key},c);
    require(result.best && result.depth==0,"expired budget supplies legal fallback");
    cout<<"All tests passed.\n";
}

// REM: Serialize the current board to FEN for the HTML application.
string exportFEN(const Position& p) {
    string fen,names=" PNBRQK";
    for(int r=7;r>=0;--r) {
        int empty=0;
        for(int f=0;f<8;++f) {
            int piece=p.board[r*8+f];
            if(!piece) { ++empty; continue; }
            if(empty) { fen+=char('0'+empty); empty=0; }
            char c=names[abs(piece)];
            fen+=piece>0?c:char(tolower(static_cast<unsigned char>(c)));
        }
        if(empty) fen+=char('0'+empty);
        if(r) fen+='/';
    }
    string rights;
    if(p.castling&1) rights+='K';
    if(p.castling&2) rights+='Q';
    if(p.castling&4) rights+='k';
    if(p.castling&8) rights+='q';
    return fen+(p.turn==WHITE?" w ":" b ")+(rights.empty()?"-":rights)
        +" "+(p.ep<0?"-":squareName(p.ep))+" "+to_string(p.halfmove)+" "+to_string(p.fullmove);
}

// REM: Emit JSON board state and legal moves for the local web bridge.
void printWebState(Position& p,const GameHistory& history,ostream& out=cout) {
    auto legal=generateLegalMoves(p);
    out<<"{\"fen\":\""<<exportFEN(p)<<"\",\"key\":\""<<p.key
        <<"\",\"turn\":\""<<(p.turn==WHITE?"w":"b")<<"\",\"board\":[";
    for(int i=0;i<64;++i) { if(i) out<<','; out<<p.board[i]; }
    out<<"],\"legal\":[";
    for(size_t i=0;i<legal.size();++i) { if(i) out<<','; out<<'"'<<moveToString(legal[i])<<'"'; }
    out<<"],\"status\":"<<getGameStatus(p,history)<<",\"check\":"
        <<(isInCheck(p,p.turn)?"true":"false")<<",\"canClaim\":"
        <<(canClaimDraw(p,history)?"true":"false")<<"}\n";
}

// REM: Read a comma-separated position history supplied by the local server.
GameHistory readWebHistory(const string& text,const Position& p) {
    GameHistory history; istringstream in(text); string part;
    while(getline(in,part,',')) if(!part.empty()) history.push_back(stoull(part));
    if(history.empty() || history.back()!=p.key) history.push_back(p.key);
    return history;
}

// REM: Dispatch terminal play, regression tests, perft, or a timed FEN search.
int engineMain(int argc,char** argv) {
    // Run interactive play by default or select a command-line utility.
    try {
        if(argc==1) { playGame(); return 0; }
        string mode=argv[1];
        // HTML bridge: state, apply a legal move, or run the timed C++ bot.
        if(mode=="--api" && argc>=5) {
            string action=argv[2]; Position p=loadFEN(argv[3]);
            auto history=readWebHistory(argv[4],p);
            if(action=="move" && argc==6) {
                auto m=parseMove(p,argv[5]);
                if(!m) throw invalid_argument("Illegal move");
                makeMove(p,*m); history.push_back(p.key); printWebState(p,history);
            } else if(action=="state" && argc==5) {
                printWebState(p,history);
            } else if(action=="bot" && argc==5) {
                SearchContext c; setBudget(c,2800,3200);
                auto result=chooseBestMove(p,history,c);
                cout<<"{\"move\":\""<<(result.best?moveToString(*result.best):"")
                    <<"\",\"claim\":"<<(result.claimDraw?"true":"false")
                    <<",\"depth\":"<<result.depth<<",\"score\":"<<result.score
                    <<",\"nodes\":"<<result.nodes<<",\"ms\":"<<result.elapsedMs<<"}\n";
            } else throw invalid_argument("Invalid API operation");
            return 0;
        }
        if(mode=="--test") { runTests(); return 0; }
        // Parse an optional FEN and the requested perft depth or search budget.
        if((mode=="--perft" || mode=="--search") && (argc==3 || argc==4)) {
            int value=stoi(argv[2]);
            Position p=argc==4?loadFEN(argv[3]):createStartingPosition();
            // Either count legal sequences or run a timed search.
            if(mode=="--perft") {
                if(value<0 || value>8) throw invalid_argument("Perft depth must be 0 through 8");
                cout<<perft(p,value)<<'\n';
            } else {
                if(value<1 || value>60000) throw invalid_argument("Search budget must be 1 through 60000 ms");
                SearchContext c; setBudget(c,value*7/8,value);
                auto r=chooseBestMove(p,{p.key},c);
                cout<<(r.claimDraw?"claim draw":r.best?moveToString(*r.best):"no move")
                    <<" depth="<<r.depth<<" score="<<r.score<<" nodes="<<r.nodes<<" ms="<<r.elapsedMs<<'\n';
            }
            return 0;
        }
        // Show valid commands when arguments do not match a supported mode.
        cerr<<"Usage: chess_bot [--test | --perft depth [FEN] | --search milliseconds [FEN]]\n";
        return 1;
    } catch(const exception& e) { cerr<<"Error: "<<e.what()<<'\n'; return 1; }
}

// ==================== LOCAL APPLICATION SERVER ====================
// REM: Close sockets through the correct operating-system API.
void closeSocket(Socket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

// REM: Decode browser form fields without third-party JSON dependencies.
string decodeForm(const string& text) {
    string out;
    auto hex=[](char c)->int {
        if(c>='0' && c<='9') return c-'0';
        if(c>='a' && c<='f') return c-'a'+10;
        if(c>='A' && c<='F') return c-'A'+10;
        return -1;
    };
    for(size_t i=0;i<text.size();++i) {
        if(text[i]=='+') out+=' ';
        else if(text[i]=='%') {
            if(i+2>=text.size() || hex(text[i+1])<0 || hex(text[i+2])<0)
                throw invalid_argument("Invalid request encoding");
            out+=char(hex(text[i+1])*16+hex(text[i+2])); i+=2;
        } else out+=text[i];
    }
    return out;
}
map<string,string> parseForm(const string& body) {
    map<string,string> fields; istringstream in(body); string pair;
    while(getline(in,pair,'&')) {
        auto split=pair.find('=');
        if(split==string::npos) throw invalid_argument("Invalid form field");
        fields[decodeForm(pair.substr(0,split))]=decodeForm(pair.substr(split+1));
    }
    return fields;
}

// REM: Escape messages before putting them into JSON strings.
string jsonString(const string& text) {
    string out="\"";
    for(unsigned char c:text) {
        if(c=='"' || c=='\\') { out+='\\'; out+=char(c); }
        else if(c=='\n') out+="\\n";
        else if(c=='\r') out+="\\r";
        else if(c=='\t') out+="\\t";
        else if(c<32) out+=' ';
        else out+=char(c);
    }
    return out+'"';
}

// REM: Validate a browser request, then call the same engine functions as terminal play.
string handleAppRequest(const string& body) {
    auto fields=parseForm(body);
    string fen=fields.count("fen")?fields["fen"]:START_FEN;
    if(fen.size()>200 || fields["history"].size()>60000) throw invalid_argument("Request is too large");
    Position p=loadFEN(fen);
    auto history=readWebHistory(fields["history"],p);
    if(history.size()>3000) throw invalid_argument("History is too long");
    string action=fields["action"]; ostringstream out;
    // Board state includes all legal destinations, so HTML never invents chess rules.
    if(action=="state") printWebState(p,history,out);
    else if(action=="move") {
        if(getGameStatus(p,history)!=PLAYING) throw invalid_argument("Game has ended");
        auto move=parseMove(p,fields["move"]);
        if(!move) throw invalid_argument("Illegal move");
        makeMove(p,*move); history.push_back(p.key); printWebState(p,history,out);
    } else if(action=="bot") {
        SearchContext context;
        // Difficulty restricts depth while every mode shares the 3.2-second ceiling.
        string difficulty=fields["difficulty"];
        context.maxDepth=difficulty=="easy"?2:difficulty=="medium"?4:95;
        setBudget(context,2800,3200);
        auto result=chooseBestMove(p,history,context);
        out<<"{\"move\":"<<jsonString(result.best?moveToString(*result.best):"")
            <<",\"claim\":"<<(result.claimDraw?"true":"false")
            <<",\"depth\":"<<result.depth<<",\"score\":"<<result.score
            <<",\"nodes\":"<<result.nodes<<",\"ms\":"<<result.elapsedMs<<"}";
    } else throw invalid_argument("Unknown operation");
    return out.str();
}

// REM: Send a full HTTP response even if the socket accepts only part at a time.
void sendResponse(Socket client,int status,const string& type,const string& body) {
    string response="HTTP/1.1 "+to_string(status)+(status==200?" OK":" Error")
        +"\r\nContent-Type: "+type+"\r\nContent-Length: "+to_string(body.size())
        +"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n"+body;
    size_t sent=0;
    while(sent<response.size()) {
        int amount=int(send(client,response.data()+sent,int(response.size()-sent),0));
        if(amount<=0) return;
        sent+=size_t(amount);
    }
}

// REM: Read one bounded HTTP request and serve only the HTML or the local engine API.
void serveClient(Socket client,int port,const string& html) {
    try {
        string request; char buffer[4096]; size_t headerEnd=string::npos;
        // Stop slow clients instead of waiting indefinitely for unfinished headers.
#ifdef _WIN32
        DWORD timeout=5000;
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
#else
        timeval timeout{5,0};
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
#endif
        while((headerEnd=request.find("\r\n\r\n"))==string::npos) {
            int n=int(recv(client,buffer,sizeof(buffer),0));
            if(n<=0) return;
            request.append(buffer,size_t(n));
            if(request.size()>65536) throw invalid_argument("Headers too large");
        }
        istringstream headers(request.substr(0,headerEnd));
        string method,path,version,line; headers>>method>>path>>version; getline(headers,line);
        map<string,string> values;
        while(getline(headers,line)) {
            if(!line.empty() && line.back()=='\r') line.pop_back();
            auto colon=line.find(':'); if(colon==string::npos) continue;
            string key=line.substr(0,colon),value=line.substr(colon+1);
            transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return char(tolower(c));});
            while(!value.empty() && value.front()==' ') value.erase(value.begin());
            values[key]=value;
        }
        // Restrict Host and Origin to localhost to keep this a personal local app.
        string host=values["host"];
        if(host!="127.0.0.1:"+to_string(port) && host!="localhost:"+to_string(port)) {
            sendResponse(client,403,"application/json","{\"error\":\"Local requests only\"}"); return;
        }
        string origin=values["origin"];
        if(!origin.empty() && origin!="http://127.0.0.1:"+to_string(port)
            && origin!="http://localhost:"+to_string(port)) {
            sendResponse(client,403,"application/json","{\"error\":\"Origin rejected\"}"); return;
        }
        if(method=="GET" && (path=="/" || path=="/index.html")) {
            sendResponse(client,200,"text/html; charset=utf-8",html); return;
        }
        if(method!="POST" || path!="/api") {
            sendResponse(client,404,"application/json","{\"error\":\"Not found\"}"); return;
        }
        if(values["content-type"].find("application/x-www-form-urlencoded")!=0)
            throw invalid_argument("Expected form data");
        size_t length=stoull(values["content-length"]);
        if(length==0 || length>64000) throw invalid_argument("Invalid request size");
        string body=request.substr(headerEnd+4);
        while(body.size()<length) {
            int n=int(recv(client,buffer,sizeof(buffer),0));
            if(n<=0) throw invalid_argument("Incomplete request");
            body.append(buffer,size_t(n));
        }
        sendResponse(client,200,"application/json",handleAppRequest(body.substr(0,length)));
    } catch(const exception& error) {
        sendResponse(client,400,"application/json","{\"error\":"+jsonString(error.what())+"}");
    }
}

// REM: Run the app on 127.0.0.1; one sequential engine search keeps resource usage bounded.
int serveApplication(int port,const filesystem::path& directory) {
#ifdef _WIN32
    WSADATA data{};
    if(WSAStartup(MAKEWORD(2,2),&data)!=0) throw runtime_error("Cannot initialize sockets");
#else
    signal(SIGPIPE,SIG_IGN);
#endif
    ifstream file(directory/"index.html",ios::binary);
    if(!file) throw runtime_error("Place index.html beside chessapp, or run from its folder");
    string html((istreambuf_iterator<char>(file)),istreambuf_iterator<char>());
    Socket listener=socket(AF_INET,SOCK_STREAM,0);
#ifdef _WIN32
    if(listener==INVALID_SOCKET) throw runtime_error("Cannot open a socket");
#else
    if(listener<0) throw runtime_error("Cannot open a socket");
#endif
    int reuse=1;
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char*>(&reuse),sizeof(reuse));
    sockaddr_in address{}; address.sin_family=AF_INET;
    address.sin_port=htons(static_cast<uint16_t>(port)); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0 || listen(listener,8)!=0) {
        closeSocket(listener); throw runtime_error("Port is busy; try --serve 8766");
    }
    cout<<"Sierra Chess\nOpen http://127.0.0.1:"<<port<<" in your browser.\nPress Ctrl+C to stop.\n"<<flush;
    while(true) {
        Socket client=accept(listener,nullptr,nullptr);
#ifdef _WIN32
        if(client==INVALID_SOCKET) break;
#else
        if(client<0) break;
#endif
        serveClient(client,port,html); closeSocket(client);
    }
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}

// REM: Start the visual app by default; preserve all original engine test commands.
int main(int argc,char** argv) {
    try {
        if(argc>1 && string(argv[1])!="--serve") return engineMain(argc,argv);
        int port=argc==3?stoi(argv[2]):8765;
        if(argc>3 || port<1024 || port>65535) throw invalid_argument("Use --serve with a port from 1024 to 65535");
        filesystem::path directory=filesystem::absolute(argv[0]).parent_path();
        if(!filesystem::exists(directory/"index.html")) directory=filesystem::current_path();
        return serveApplication(port,directory);
    } catch(const exception& error) { cerr<<"Error: "<<error.what()<<'\n'; return 1; }
}
