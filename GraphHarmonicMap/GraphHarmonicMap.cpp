#include <cstdlib>
#include <omp.h>
#include <queue>
#include <set>
#include <ctime>
#include "GraphHarmonicMap.h"
#include "Parser/parser.h"
#include "Eigen/Eigen"

CGraphHarmonicMap::CGraphHarmonicMap()
{
    mesh = new CMesh();
    graph = new CGraph();
}


CGraphHarmonicMap::~CGraphHarmonicMap()
{
}

int CGraphHarmonicMap::setMesh(const string & filename)
{
    mesh->read_m(filename.c_str());
    mesh->prop("filename") = filename;
    return 0;
}

/*
* read a target graph, place source vertex randomly on the graph: arc is chosen randomly, always place target at the middle of each arc
*/
int CGraphHarmonicMap::setGraph(const string & graphfilename, const string & cutfilename = string())
{
    graph->read(graphfilename);
    graph->prop("filename") = graphfilename;
    graph->prop("cutfilename") = cutfilename;
    if (!mesh)
    {
        cerr << "read mesh first" << endl;
        exit(-1);
        return -1;
    }

    ifstream cutfile(cutfilename);
    if (cutfile.good())
    {
        map<int, int> seeds;
        string line;
        while (getline(cutfile, line))
        {
            std::istringstream iss(line);
            string type;
            int id, sign, vid;
            iss >> type;
            if (type[0] == '#') continue;
            if (type == "Cut")
            {
                iss >> id >> sign;
                auto edge = graph->g.edgeFromId(id);
                graph->sign[edge] = sign;
                vector<int> cut;
                while (iss.good())
                {
                    iss >> vid;
                    cut.push_back(vid);
                }
                cuts[id] = cut;
            }
            if (type == "Seed")
            {
                int id, seed;
                iss >> id >> seed;
                seeds[id] = seed;
            }
        }
        initialMap(cuts, seeds);
        return 0;
    }

    vector<SmartGraph::Edge> edges;
    for (SmartGraph::EdgeIt e(graph->g); e != INVALID; ++e)
    {
        edges.push_back(e);
    }

    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        string s = v->string();
        CParser parser(s);
        list<CToken*> & tokens = parser.tokens();

        for (auto tit = tokens.begin(); tit != tokens.end(); ++tit)
        {
            CToken * pt = *tit;
            if (pt->m_key == "target")
            {
                string str = strutil::trim(pt->m_value, "()");
                istringstream iss(str);
                int edgeId = 0;
                int nodeId = 0;
                double length = 0.0;
                iss >> edgeId >> nodeId >> length;
                CTarget * t = new CTarget();
                t->edge = graph->g.edgeFromId(edgeId);
                t->node = graph->g.u(t->edge);
                t->length = length;
                v->prop("target") = t;
            }
            else
            {
                cout << "vertex target is needed" << endl;
                exit(-1);
                return -1;
            }
        }
    }

    return 0;
}

int CGraphHarmonicMap::calculateEdgeLength()
{
    for (MeshEdgeIterator eit(mesh); !eit.end(); ++eit)
    {
        CEdge * e = *eit;
        CVertex * v1 = e->halfedge(0)->source();
        CVertex * v2 = e->halfedge(0)->target();
        e->length() = (v1->point() - v2->point()).norm();
    }
    return 0;
}

int CGraphHarmonicMap::calculateEdgeWeight()
{
    auto inverse_cosine_law = [](double li, double lj, double lk) { return acos((lj * lj + lk * lk - li * li) / (2 * lj * lk)); };
    for (MeshEdgeIterator eit(mesh); !eit.end(); ++eit)
    {
        CEdge * e = *eit;
        e->prop("weight") = double(0.0);
        CHalfEdge * he0 = e->halfedge(0);
        CHalfEdge * he1 = e->halfedge(1);
        if (he0) he0->touched() = false;
        if (he1) he1->touched() = false;
    }
    for (MeshHalfEdgeIterator heit(mesh); !heit.end(); ++heit)
    {
        CHalfEdge * he = *heit;
        if (he->touched()) continue;
        CHalfEdge * he_next = he->he_next();
        CHalfEdge * he_prev = he->he_prev();
        double theta1 = inverse_cosine_law(he->edge()->length(), he_next->edge()->length(), he_prev->edge()->length());
        he->edge()->prop("weight") = 0.5 / tan(theta1);
        he->touched() = true;

        CHalfEdge * he_dual = he->he_sym();
        if (!he_dual || he_dual->touched()) continue;
        CHalfEdge * he_dual_next = he_dual->he_next();
        CHalfEdge * he_dual_prev = he_dual->he_prev();
        double theta2 = inverse_cosine_law(he_dual->edge()->length(), he_dual_next->edge()->length(), he_dual_prev->edge()->length());
        he->edge()->prop("weight") = 0.5 / tan(theta1) + 0.5 / tan(theta2);
        he_dual->touched() = true;
    }
    return 0;
}

/*
 * find the minimum point of a quadratic function in interval [x0, x1]
 */
inline double quadraticMininum(double a, double b, double c, double x0, double x1, double &x)
{
    auto func = [ = ](double x) { return a * x * x + b * x + c; };

    double f0 = func(x0);
    double f1 = func(x1);
    x = x0;
    double fm = f0;
    if (f1 < f0)
    {
        x = x1;
        fm = f1;
    }
    double x2 = -b / a / 2.0;
    double f2 = func(x2);
    if (x0 < x2 && x2 < x1)
    {
        if (f2 < fm)
        {
            x = x2;
            fm = f2;
        }
    }
    return fm;
}

/*
 * distance between two points x and y on the graph
 */
double CGraphHarmonicMap::distance(CTarget * x, CTarget * y)
{
    // first compute the shorest distance between the node of two edges
    auto & ex = x->edge;
    auto & ey = y->edge;
    double elx = graph->edgeLength[ex];
    double ely = graph->edgeLength[ey];
    auto & ux = graph->g.u(ex);
    auto & vx = graph->g.v(ex);
    auto & uy = graph->g.u(ey);
    auto & vy = graph->g.v(ey);
    SmartGraph::Node nx, ny;
    double dey = distance(x, ey, nx, ny);
    if (uy == vy) // y is a loop
    {
        if (ex == ey)
        {
            dey = fabs(x->length - y->length);
            if (dey > ely / 2.0) dey = ely - dey;
        }
        else
        {
            if (y->length > ely / 2.0) dey += ely - y->length;
            else dey += y->length;
        }
    }
    else
    {
        if (ex == ey) // x, y on same edge
        {
            dey = fabs(x->length - y->length);
        }
        else
        {
            // ny is the node nearer to x, if it is the starting node of y
            if (ny == uy) dey = dey + y->length;
            else dey += ely - y->length;
        }

    }
    return dey;
}

double CGraphHarmonicMap::distance(CTarget * x, const SmartGraph::Edge & e, SmartGraph::Node & nx, SmartGraph::Node & ne)
{
    auto u = graph->g.u(e);
    auto v = graph->g.v(e);

    SmartGraph::Node ux, vx;
    if (u == v) // e is a loop
    {
        double du = distance(x, u, ux);
        nx = ux;
        ne = u;
        return du;
    }

    double du = distance(x, u, ux);
    double dv = distance(x, v, vx);
    if (du < dv)
    {
        nx = ux;
        ne = u;
        return du;
    }
    else
    {
        nx = vx;
        ne = v;
        return dv;
    }
}

double CGraphHarmonicMap::distance(CTarget * x, const SmartGraph::Node & n, SmartGraph::Node & nx)
{
    bool isLoop = false;
    auto e = x->edge;
    double el = graph->edgeLength[e];

    auto u = graph->g.u(e);
    auto v = graph->g.v(e);



    if (u == v)
    {
        double dx = x->length;
        if (dx > el / 2.0) dx = el - dx;
        double d = graph->distance(n, u);
        nx = u;
        return dx + d;
    }
    else
    {
        double d1 = graph->distance(n, u);
        double d2 = graph->distance(n, v);
        if (x->node == u)
        {
            double dx1 = x->length + d1;
            double dx2 = el - x->length + d2;
            if (dx1 < dx2)
            {
                nx = u;
                return dx1;
            }
            else
            {
                nx = v;
                return dx2;
            }
        }
        else
        {
            double dx1 = el - x->length + d1;
            double dx2 = x->length + d2;
            if (dx1 < dx2)
            {
                nx = u;
                return dx1;
            }
            else
            {
                nx = v;
                return dx2;
            }
        }
    }
}

double CGraphHarmonicMap::distance(CTarget * x, SmartGraph::Node n)
{
    SmartGraph::Node nx;
    return distance(x, n, nx);
}


double CGraphHarmonicMap::calculateBarycenter(CVertex * v, vector<CVertex*> & nei)
{
    //vector<CVertex*> nei;
    vector<CTarget*> neit;
    vector<double> ew;
    for (auto * vj : nei)
    {
        void * t = vj->prop("target");
        CTarget * vt = (CTarget*)t;
        neit.push_back(vt);
        CEdge * e = mesh->vertexEdge(v, vj);
        double w = e->prop("weight");
        ew.push_back(w);
    }
    double a = 0.0;
    for (double w : ew) a += w;

    vector<double> fm;
    vector<CTarget*> vx;
    for (SmartGraph::EdgeIt e(graph->g); e != INVALID; ++e)
    {
        auto u = graph->g.u(e);
        auto v = graph->g.v(e);
        double el = graph->edgeLength[e];
        vector<bool> be;
        vector<double> bp = { 0, el / 2.0, el };
        for (auto it = neit.begin(); it != neit.end(); ++it)
        {
            CTarget * vt = *it;
            bool b = vt->edge == e;
            if (b)
            {
                if (vt->length < el / 2.0) bp.push_back(vt->length + el / 2.0);
                else bp.push_back(vt->length - el / 2.0);
            }
            be.push_back(b);
        }
        if (u == v) // if e is a loop
        {
            sort(bp.begin(), bp.end());
            vector<double> bx0;
            for (int i = 0; i < neit.size(); ++i)
            {
                if (be[i])
                {
                    double x = -neit[i]->length;
                    bx0.push_back(x);
                }
                else
                {
                    double dy = distance(neit[i], u);
                    bx0.push_back(dy);
                }
            }
            // find minimum point in this loop
            CTarget * t = new CTarget();
            t->edge = e;
            t->node = u;
            t->length = 0.0;
            double ei = 1.0 / EPS;
            for (int i = 0; i < bp.size() - 1; ++i)
            {
                vector<double> bx(bx0);
                double x0 = bp[i];
                double x1 = bp[i + 1];
                double xm = (x0 + x1) / 2.0;

                double b = 0.0, c = 0.0;
                for (int j = 0; j < bx.size(); ++j)
                {
                    if (be[j])
                    {
                        if (xm + bx[j] > el / 2.0) bx[j] -= el;
                        else if (xm + bx[j] < -el / 2.0) bx[j] += el;
                    }
                    else
                    {
                        if (xm > el / 2.0) bx[j] = -bx[j] - el;
                    }
                    b += 2 * ew[j] * bx[j];
                    c += ew[j] * bx[j] * bx[j];
                }
                double x = 0;
                double mi = quadraticMininum(a, b, c, x0, x1, x);
                if (mi < ei)
                {
                    ei = mi;
                    t->length = x;
                }
            }
            fm.push_back(ei);
            vx.push_back(t);
        }

        else // if e is not loop
        {
            vector<double> bx;
            for (int i = 0; i < neit.size(); ++i)
            {
                if (be[i])
                {
                    double x = -neit[i]->length;
                    bx.push_back(x);
                }
                else
                {
                    double du = distance(neit[i], u);
                    double dv = distance(neit[i], v);
                    if (du <= dv) bx.push_back(du);
                    else bx.push_back(-dv - el);
                }
            }
            double b = 0.0, c = 0.0;
            for (int j = 0; j < bx.size(); ++j)
            {
                b += 2 * ew[j] * bx[j];
                c += ew[j] * bx[j] * bx[j];
            }
            CTarget * t = new CTarget();
            t->edge = e;
            t->node = u;
            t->length = 0.0;
            double x = 0;
            double mi = quadraticMininum(a, b, c, 0, el, x);
            t->length = x;
            fm.push_back(mi);
            vx.push_back(t);
        }

    }

    double fmm = fm[0];
    CTarget * vm = vx[0];
    for (int i = 1; i < fm.size(); ++i)
    {
        if (fm[i] < fmm)
        {
            fmm = fm[i];
            vm = vx[i];
        }
    }
    void * t = v->prop("target");
    CTarget * vt = (CTarget*)t;
    double dv = distance(vm, vt);
    if (dv > 0.01)
    {
        CVertex *v2 = v;
    }
    vt->edge = vm->edge;
    vt->node = vm->node;
    vt->length = vm->length;

    for (int i = 0; i < vx.size(); ++i)
    {
        delete vx[i];
    }
    return dv;
}

/*
 * initial map consists of ordinary harmonic map from pants to a "Y" graph
 */
int CGraphHarmonicMap::initialMap(Cut & cuts, map<int, int> & seeds)
{
    calculateEdgeLength();
    calculateEdgeWeight();
    // renumber vertex
    int i = 0;
    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        v->prop("id") = i++;
        v->prop("cut") = false;
        v->prop("cut2") = false;
        v->prop("x") = double(0.0);
        v->prop("y") = double(0.0);
    }
    // label cuts
    for (auto c : cuts)
    {
        int id = c.first;
        auto vs = c.second;
        for (auto id : vs)
        {
            CVertex * v = mesh->idVertex(id);
            v->prop("cut") = true;
        }
    }
    // trace pants
    map<int, vector<CVertex*>> pantss;
    traceAllPants(seeds, pantss);

    for (auto p : pantss)
    {
        int id = p.first;
        auto pants = p.second;
        auto node = graph->g.nodeFromId(id);
        embedPants(node, pants);
    }

    //return 0;
    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        void * t = v->prop("target");
        CTarget * vt = (CTarget*)t;
        auto edge = vt->edge;
        auto node = vt->node;
        double length = vt->length;
        if (node != graph->g.u(edge))
        {
            vt->node = graph->g.u(edge);
            vt->length = graph->edgeLength[edge] - length;
        }
    }
    return 0;
}

/*
 * for every vertex, move it to its neighbors' weighted center
*/
int CGraphHarmonicMap::harmonicMap()
{
    vector<CVertex*> vv;
    vector<vector<CVertex*>> neis;

    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        vv.push_back(v);
        vector<CVertex*> nei;
        for (VertexVertexIterator vit(v); !vit.end(); ++vit)
        {
            CVertex * vj = *vit;;
            nei.push_back(vj);
        }
        neis.push_back(nei);
    }
    time_t start = clock();
    int k = 0;
    while (k++ < 1000)
    {
        double err = 0;
        //random_shuffle(vv.begin(), vv.end());
        #pragma omp parallel for
        for (int i = 0; i < vv.size(); ++i)
        {
            double d = calculateBarycenter(vv[i], neis[i]);
            if (d > err) err = d;
        }
        if (k % 100 == 0) cout << "#" << k << ": " << err << endl;
        if (k % 500 == 0) writeMap("harmonic." + to_string(int(k / 500)));
        if (err < EPS) break;
    }
    time_t finish = clock();
    cout << "elapsed time is " << (finish - start) / double(CLOCKS_PER_SEC) << endl;
    return 0;
}

int CGraphHarmonicMap::traceAllPants(const map<int, int> & seeds, map<int, vector<CVertex*>> & pantss)
{
    for (auto s : seeds)
    {
        int id = s.first;
        int seed = s.second;
        vector<CVertex*> pants;
        tracePants(id, seed, pants);
        pantss[id] = pants;
    }
    return 0;
}

int CGraphHarmonicMap::tracePants(int id, int seed, vector<CVertex*> & pants)
{
    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        v->touched() = false;
    }
    queue<CVertex*> qe;
    CVertex * s = mesh->idVertex(seed);
    qe.push(s);
    pants.clear();
    while (!qe.empty())
    {
        CVertex * v = qe.front();
        qe.pop();
        pants.push_back(v);
        v->touched() = true;
        v->prop("pants") = id;
        bool isCut = v->prop("cut");
        if (isCut) continue;
        for (VertexVertexIterator vvit(v); !vvit.end(); ++vvit)
        {
            CVertex * vj = *vvit;
            if (vj->touched()) continue;
            bool isCut = vj->prop("cut");
            if (!isCut) qe.push(vj);
            else pants.push_back(vj);
            vj->touched() = true;
        }
    }
    return 0;
}

/*
 * embed pants by computing a harmonic map from pants to a "Y" shape graph
 */
int CGraphHarmonicMap::embedPants(SmartGraph::Node & node, vector<CVertex*> & pants)
{
    vector<SmartGraph::Edge> edges;
    for (SmartGraph::OutArcIt oa(graph->g, node); oa != INVALID; ++oa)
    {
        SmartGraph::Edge e = oa;
        edges.push_back(e);
    }

    if (edges.size() != 3)
    {
        cerr << "not a valid pants decomposition" << endl;
        exit(-1);
        return -1;
    }
    // if there is a loop
    if (edges[0] == edges[1])
    {
        embedPants(node, pants, edges[0], edges[1], edges[2]);
    }
    else if (edges[0] == edges[2] )
    {
        embedPants(node, pants, edges[0], edges[2], edges[1]);
    }
    else if (edges[1] == edges[2])
    {
        embedPants(node, pants, edges[1], edges[2], edges[0]);
    }
    else
    {
        embedPants(node, pants, edges[0], edges[1], edges[2]);
    }

    return 0;
}

/*
 * e0 may equal to e1, but not to e2
 */
int CGraphHarmonicMap::embedPants(SmartGraph::Node & node, vector<CVertex*> & pants, SmartGraph::Edge & e0, SmartGraph::Edge & e1, SmartGraph::Edge & e2)
{
    // renumber pants vertex
    int i = 0;
    for (auto v : pants)
    {
        v->prop("id") = i++;
    }

    if (e0 == e1)
    {
        int eid = graph->g.id(e0);
        double length = graph->edgeLength[e0];
        auto & cut = cuts[eid];
        vector<CVertex*> vs1, vs2;
        findNeighbors(cut, vs1, vs2);

        // find two side neighbors of cut
        for (auto v : vs1)
        {
            v->prop("x") = -length / 2.0;
            v->prop("y") = double(0.0);
            v->prop("cut2") = true;
            v->touched() = true;
        }
        for (auto v : vs2)
        {
            v->prop("x") = double(0.0);
            v->prop("y") = -length / 2.0;
            v->prop("cut2") = true;
            v->touched() = true;
        }
        for (auto i : cut)
        {
            CVertex * v = mesh->idVertex(i);
            v->prop("x") = -length / 2.0;
            v->prop("y") = -length / 2.0;
            v->prop("cut2") = true;
            v->touched() = true;
        }
    }
    else
    {
        int e0id = graph->g.id(e0);
        auto & cut0 = cuts[e0id];
        double l0 = graph->edgeLength[e0];
        for (auto i : cut0)
        {
            CVertex * v = mesh->idVertex(i);
            v->prop("x") = -l0 / 2.0;
            v->prop("y") = double(0.0);
            v->prop("cut2") = true;
            v->touched() = true;
        }

        int e1id = graph->g.id(e1);
        auto & cut1 = cuts[e1id];
        double l1 = graph->edgeLength[e1];
        for (auto i : cut1)
        {
            CVertex * v = mesh->idVertex(i);
            v->prop("x") = double(0.0);
            v->prop("y") = -l1 / 2.0;
            v->prop("cut2") = true;
            v->touched() = true;
        }
    }

    int eid = graph->g.id(e2);
    double length = graph->edgeLength[e2];
    auto & cut = cuts[eid];
    for (auto i : cut)
    {
        CVertex * v = mesh->idVertex(i);
        v->prop("x") = length / 2.0;
        v->prop("y") = length / 2.0;
        v->prop("cut2") = true;
        v->touched() = true;
    }

    // compute harmonic map
    typedef Eigen::Triplet<double> T;
    vector<T> triplets, tripletsb;
    int nv = pants.size();
    triplets.reserve(nv * 2);
    Eigen::SparseMatrix<double> A(nv, nv);
    Eigen::SparseMatrix<double> B(nv, nv);
    Eigen::VectorXd x, y;
    Eigen::VectorXd bx, by;
    bx.setZero(nv, 1);
    by.setZero(nv, 1);

    for (auto v : pants)
    {
        int id = v->prop("id");
        bool isCut = v->prop("cut");
        bool isCut2 = v->prop("cut2");
        bool c = isCut || isCut2;
        if (!c)
        {
            for (VertexVertexIterator vvit(v); !vvit.end(); ++vvit)
            {
                CVertex * v2 = *vvit;
                int id2 = v2->prop("id");
                bool isCut = v2->prop("cut");
                bool isCut2 = v2->prop("cut2");
                bool c2 = isCut || isCut2;
                CEdge * e = mesh->vertexEdge(v, v2);
                double w = e->prop("weight");
                triplets.push_back(T(id, id, -w));
                if (!c2)
                {
                    triplets.push_back(T(id, id2, w));
                }
                else
                {
                    tripletsb.push_back(T(id, id2, w));
                }
            }
        }
        else
        {
            triplets.push_back(T(id, id, 1));
            double x = v->prop("x");
            double y = v->prop("y");
            bx[id] = x;
            by[id] = y;
        }
    }
    ofstream ofsx("x");
    ofsx << bx;
    ofstream ofsy("y");
    ofsy << by;

    A.setFromTriplets(triplets.begin(), triplets.end());
    A.finalize();

    B.setFromTriplets(tripletsb.begin(), tripletsb.end());
    B.finalize();

    bx -= B * bx;
    by -= B * by;

    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver;
    solver.compute(A);
    if (solver.info() != Eigen::Success)
    {
        cerr << "waring: Eigen decomposition failed" << endl;
    }
    x = solver.solve(bx);
    y = solver.solve(by);



    for (auto v : pants)
    {
        int id = v->prop("id");
        double xi = x[id];
        double yi = y[id];
        v->prop("x") = xi;
        v->prop("y") = yi;
    }

    for (auto v : pants)
    {
        double x = v->prop("x");
        double y = v->prop("y");
        CTarget * t = new CTarget();
        t->node = node;
        if (x >= 0 && y >= 0)
        {
            t->edge = e2;
            t->length = (x + y) / 2.0;
        }
        else if (x < y)
        {
            t->edge = e0;
            t->length = -x;
        }
        else if (y <= x)
        {
            t->edge = e1;
            t->length = -y;
        }
        v->prop("target") = t;
    }

    return 0;
}

int CGraphHarmonicMap::findNeighbors(vector<int> & cut, vector<CVertex*> & vs1, vector<CVertex*> & vs2)
{
    vs1.clear();
    vs2.clear();
    for (int i = 0; i < cut.size() - 1; ++i)
    {
        CVertex * v0 = mesh->idVertex(cut[i]);
        CVertex * v1 = mesh->idVertex(cut[i + 1]);
        CEdge * e = mesh->vertexEdge(v0, v1);
        CHalfEdge * he0 = e->halfedge(0);
        CHalfEdge * he1 = e->halfedge(1);
        CHalfEdge * he = NULL;
        if (he0->source() == v0 && he0->target() == v1)  he = he0;
        if (he1 && he1->source() == v0 && he1->target() == v1) he = he1;
        if (!he)
        {
            cerr << "cut can be be on boundary" << endl;
            return -1;
        }
        vs1.push_back(he->he_next()->target());
        vs2.push_back(he->he_sym()->he_next()->target());
    }
    return 0;
}

void CGraphHarmonicMap::test()
{
    vector<CVertex*> vv;
    vector<vector<CVertex*>> neis;

    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        vv.push_back(v);
        vector<CVertex*> nei;
        for (VertexVertexIterator vit(v); !vit.end(); ++vit)
        {
            CVertex * vj = *vit;;
            nei.push_back(vj);
        }
        neis.push_back(nei);
    }
    auto v0 = mesh->idVertex(868);
    auto v1 = mesh->idVertex(100);
    void * x = v0->prop("target");
    void * y = v1->prop("target");
    CTarget * tx = (CTarget*)x;
    CTarget * ty = (CTarget*)y;
    double d = distance(tx, ty);

    double db = calculateBarycenter(vv[867], neis[867]);
    harmonicMap();
}

int CGraphHarmonicMap::writeMap(string filename = string())
{
    int i = filename.find_last_of('.');
    filename = filename.substr(0, i);
    filename += ".harmonic";

    ofstream map(filename);
    for (MeshVertexIterator vit(mesh); !vit.end(); ++vit)
    {
        CVertex * v = *vit;
        void * t = v->prop("target");
        CTarget * vt = (CTarget*)t;
        CPoint & p = v->point();
        auto e = vt->edge;
        int sign = graph->sign[e];
        double el = graph->edgeLength[e];
        double x = vt->length;
        //if (x > el / 2.0) x = (el - x) * sign;
        //else x = x * sign;
        map << graph->g.id(vt->edge) << " " << graph->g.id(vt->node) << " " << x << endl;
    }

    return 0;
}