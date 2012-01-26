/****************************************************************************[RelativeInduction.cc]
Copyright (c) 2011, Niklas Sorensson
Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
associated documentation files (the "Software"), to deal in the Software without restriction,
including without limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
**************************************************************************************************/

#include "minisat/mtl/Alg.h"
#include "minisat/mtl/Sort.h"
#include "minisat/utils/System.h"
#include "tip/induction/Induction.h"
#include "tip/induction/TripTypes.h"
#include "tip/induction/TripProofInstances.h"
#include "tip/liveness/EmbedFairness.h"
#include "tip/unroll/Bmc.h"

#define GENERALIZE_THEN_PUSH
//#define VERIFY_SUBSUMPTION
//#define VERIFY_INVARIANT

namespace Tip {

    namespace {

        // Helper to calculate the "Luby" sequence for restart intervals:
        double luby(double y, int x){
            // Find the finite subsequence that contains index 'x', and the
            // size of that subsequence:
            int size, seq;
            for (size = 1, seq = 0; size < x+1; seq++, size = 2*size+1);
            
            while (size-1 != x){
                size = (size-1)>>1;
                seq--;
                x = x % size;
            }
            
            return pow(y, seq);
        }

        struct EventCounter {
            unsigned k;
            Sig      x;
            EventCounter() : k(0), x(sig_True){}
        };

        //===================================================================================================
        // Temporal Relative Induction Prover:

        BoolOption opt_fwd_revive   ("RIP", "rip-fwd-rev",  "Use revival of forward-subsumed clauses", false);
        BoolOption opt_bwd_revive   ("RIP", "rip-bwd-rev",  "Use revival of backward-subsumed clauses", false);
        BoolOption opt_fwd_inst     ("RIP", "rip-fwd-inst", "Instantiate proved clauses multiple cycles", true);
        BoolOption opt_order_heur   ("RIP", "rip-order",    "Use generalization order heuristic", false);
        IntOption  opt_restart      ("RIP", "rip-restart",  "Use this interval for rip-engine restarts (0=off)", 0);
        BoolOption opt_restart_luby ("RIP", "rip-restart-luby", "Use luby sequence for rip-engine restarts", false);
        IntOption  opt_max_gen_tries("RIP", "rip-gen-tries","Max number of tries in clause generalization", 32);
        IntOption  opt_cnf_level    ("RIP", "rip-cnf", "Effort level for CNF simplification (0-2)", 1, IntRange(0,2));

        class Trip {
            TipCirc&             tip;

            // Solver independent data:
            vec<vec<Clause*> >   F;             // Proved clauses (by cycle).
            vec<unsigned>        F_size;        // Number of active clauses in each cycle.
            vec<Clause*>         F_inv;         // Invariant clauses.
            unsigned             n_inv;         // Number of active invariants.
            unsigned             n_total;       // Total number of active clauses.
            GMap<int>            num_occ;       // Number of clause occurences for each gate.
            uint32_t             luby_index;    // Luby sequence index.
            uint32_t             restart_cnt;   // Restart bound counter.

            vec<vec<SharedRef<ScheduledClause> > >
                                 clause_queue;
            SMap<vec<Clause*> >  bwd_occurs;
            SMap<vec<Clause*> >  fwd_occurs;

            // Solver data: Should be rederivable from only independent data at any time:
            InitInstance2        init;
            PropInstance         prop;
            StepInstance         step;

            // Liveness to safety mapping:
            vec<EventCounter>    event_cnts;

            // Options:
            bool                 fwd_revive;
            bool                 bwd_revive;
            bool                 fwd_inst;
            bool                 order_heur;
            uint32_t             restart_ival;
            bool                 restart_luby;
            uint32_t             max_gen_tries;

            // Statistics:
            double               cpu_time;
            uint64_t             cls_added;
            uint64_t             cls_moved;
            uint64_t             cls_bwdsub;
            uint64_t             cls_revived;
            uint64_t             cls_total_size;
            uint64_t             cls_total_before;
            uint64_t             cls_total_removed;

            uint64_t             cands_added;
            uint64_t             cands_fwdsub;
            uint64_t             cands_revived;
            uint64_t             cands_total_size;
            uint64_t             cands_total_removed;

            // PROVE:   let k = c.cycle: F_inv ^ F[k-1] ^ c ^ Trans => c'
            // RETURNS: True and a minimal stronger clause d (subset of c) that holds in a maximal cycle >= k,
            //       or False and a new clause predecessor to be proved in cycle k-1.
            bool             proveAndGeneralize(SharedRef<ScheduledClause> c, Clause& yes, SharedRef<ScheduledClause>& no);

            // PROVE:   let k = c.cycle: F_inv ^ F[k-1] ^ c ^ Trans => c'
            // RETURNS: True and a stronger clause d (subset of c) that holds in some cycle >= k,
            //       or False if c does not hold in cycle k.
            bool             proveStep(const Clause& c, Clause& yes);

            void             scheduleGeneralizeOrder(const Clause& c, vec<Sig>& try_remove);

            // Find a maximal generalization of c that still is subsumed by init.
            void             generalize(Clause& c);

            // Find a maximal generalization of c that holds in initial states.
            void             generalizeInit(Clause& c);

            // PROVE:   let k = F.size()-1: F_inv ^ F[k] ^ Trans => p'
            // RETURNS: l_True if the property is implied by the invariants alone,
            //       or l_False and a new clause predecessor to be proved in cycle k,
            //       or l_Undef when the propery 'p' holds in cycle k+1.
            lbool            proveProp(Sig p, SharedRef<ScheduledClause>& no);

            // Prove scheduled clause "recursively". Returns true if the clause was proved, and
            // false if it was falsified.
            bool             proveRec(SharedRef<ScheduledClause> sc, SharedRef<ScheduledClause>& pred);

            // Try to push clauses forwards, particularily push clauses forward into the newly opened last
            // frame. Returns true if an invariant is found and false otherwise.
            void             pushClauses  ();

            // Add a proved clause 'c'. Returns true if this causes an invariant to be found, and false
            // otherwise.
            bool             addClause    (const Clause& c);

            // When some set of invariants have been found, extract and add the clauses to
            // 'F_inv'. Also perform backward subsumption to remove redundant clauses.
            void             extractInvariant();

            // Remove a proved clause 'c'. Returns true if this causes an invariant to be found,
            // and false otherwise.
            bool             removeClause (Clause* c);

            void             clearInactive();

            // Check if clause 'c' is subsumed by any previously proved clause. Returns true if it is and
            // false otherwise.
            bool             fwdSubsumed  (const Clause* c, unsigned& cycle);
            bool             fwdSubsumed  (const Clause* c);

            // Find and remove all previously proved claues that are subsumed by the new clause 'c'. If an
            // invariant is found true is returned, otherwise false.
            bool             bwdSubsume   (Clause* c, bool verify = false);
            void             verifySubsumption();

            void             enqueueClause(SharedRef<ScheduledClause> sc);
            SharedRef<ScheduledClause>
                             getMinClause ();

            // Extracts the trace that leads to the failure, and removes all scheduled clauses.
            void             extractTrace (SharedRef<ScheduledClause> sc, vec<vec<lbool> >& frames);


            void             extendLiveness(LiveProp p);



            // Returns number of states in unrolling.
            unsigned         size () const;

            // DEBUG:
            void             printClause (const Clause& c);

            
        public:

            void             printInvariant  ();
            void             verifyInvariant ();

            Trip(TipCirc& t) : tip(t), n_inv(0), n_total(0), num_occ(tip.main.lastGate(), 0), luby_index(0), restart_cnt(0), init(t, opt_cnf_level), prop(t, F, opt_cnf_level), step(t, F, opt_cnf_level), 

                               fwd_revive   (opt_fwd_revive),
                               bwd_revive   (opt_bwd_revive),
                               fwd_inst     (opt_fwd_inst),
                               order_heur   (opt_order_heur),
                               restart_ival (opt_restart),
                               restart_luby (opt_restart_luby),
                               max_gen_tries(opt_max_gen_tries),

                               cpu_time  (0),

                               cls_added (0),
                               cls_moved (0),
                               cls_bwdsub(0),
                               cls_revived(0),
                               cls_total_size(0),
                               cls_total_before(0),
                               cls_total_removed(0),

                               cands_added        (0),
                               cands_fwdsub       (0),
                               cands_revived      (0),
                               cands_total_size   (0),
                               cands_total_removed(0)

            {
                F.push();
                F_size.push(0);
            }

            ~Trip()
            {
                for (int i = 0; i < F.size(); i++)
                    for (int j = 0; j < F[i].size(); j++)
                        delete F[i][j];
                
                for (int i = 0; i < F_inv.size(); i++)
                    delete F_inv[i];
            }

            // Prove or disprove all properties using depth k. Returns true if all properties are decided, and
            // false if there are still some unresolved property.
            bool decideCycle();

            // Prove that the necessary number of initial cycles are bug free. Returns true if all properties
            // are resolved (i.e. all properties were falsifiable), and false otherwise.
            bool baseCase();

            uint64_t props();
            uint64_t solves();
            double   time ();

            // Returns number of cycles proved safe (just for comparison with bmc).
            int depth() const;


            void printStats(unsigned curr_cycle = cycle_Undef, bool newline = true);
            void printFinalStats();
        };

        class SigCmp {
            const GMap<int>& num_occ;
        public:            
            SigCmp(const GMap<int>& num_occ_) : num_occ(num_occ_){}

            bool operator()(Sig x, Sig y) const { 
                assert(num_occ.has(gate(x)));
                assert(num_occ.has(gate(y)));
                int num_x = num_occ[gate(x)];
                int num_y = num_occ[gate(y)];
                //return num_x < num_y || (num_x == num_y && x < y); }
                return num_x < num_y; }
        };

        void Trip::scheduleGeneralizeOrder(const Clause& c, vec<Sig>& try_remove)
        {
            for (unsigned i = 0; i < c.size(); i++)
                try_remove.push(c[i]);
            if (order_heur)
                sort(try_remove, SigCmp(num_occ));
        }

        void Trip::generalize(Clause& c)
        {
            vec<Sig> try_remove;
            Clause   d = c;
            Clause   e;
            scheduleGeneralizeOrder(c, try_remove);

            if (tip.verbosity >= 4){
                printf("[generalize] begin d = ");
                printClause(d);
                printf("\n"); }
            for (unsigned tries = 0; tries < max_gen_tries; tries++){
                bool     repeat      = false;
                bool     failed      = false;
                unsigned size_before = d.size();
                for (int i = 0; d.size() > 1 && i < try_remove.size(); i++)
                    if (find(d, try_remove[i])){
                        Clause cand = d - try_remove[i];
                        if (tip.verbosity >= 4){
                            printf("[generalize] cand = ");
                            printClause(cand);
                            printf("\n"); }
                        if (step.prove(cand, e) && init.prove(cand, e, d)){
                            if (failed)
                                repeat = true;
                            assert(subsumes(d, cand));
                            if (tip.verbosity >= 4){
                                printf("[generalize] refine d = ");
                                printClause(d);
                                printf("\n"); }
                        }else
                            failed = true;
                    }

#if 0
                static int total = 0;
                static int skipped = 0;

                total++;
                if (!repeat)
                    skipped++;

                if (!repeat)
                    printf("[generalize] retry skipped (%4.1f %%).\n", skipped*100 / (double)total);
#endif

                if (tip.verbosity >= 2 && tries > 0 && d.size() < size_before)
                    printf("[generalize] retry %d at cycle %d shrunk with %d                     \n", 
                           tries, d.cycle, size_before - d.size());

                if (!repeat) break;
            }
            
            assert(subsumes(d, c));
            c = d;
            if (tip.verbosity >= 4){
                printf("[generalize] done c = ");
                printClause(c);
                printf("\n");}
            // assert(init.prove(c, c, e));
            // assert(step.prove(c, e));
        }


        void Trip::generalizeInit(Clause& c)
        {
            assert(c.cycle == 0);
            vec<Sig> try_remove;
            Clause   d = c;
            Clause   empty;
            scheduleGeneralizeOrder(c, try_remove);

            if (tip.verbosity >= 4){
                printf("[generalizeInit] begin d = ");
                printClause(d);
                printf("\n"); }

            for (int i = 0; d.size() > 1 && i < try_remove.size(); i++)
                if (find(d, try_remove[i])){
                    Clause cand = d - try_remove[i];
                    if (tip.verbosity >= 4){
                        printf("[generalizeInit] cand = ");
                        printClause(cand);
                        printf("\n"); }
                    if (init.prove(cand, empty, d)){
                        assert(subsumes(d, cand));
                        if (tip.verbosity >= 4){
                            printf("[generalizeInit] refine d = ");
                            printClause(d);
                            printf("\n"); }
                    }
                }
            assert(subsumes(d, c));
            c = d;
            if (tip.verbosity >= 4){
                printf("[generalizeInit] done c = ");
                printClause(c);
                printf("\n");}
        }


        bool Trip::proveAndGeneralize(SharedRef<ScheduledClause> c, Clause& yes, SharedRef<ScheduledClause>& no)
        {
            Clause yes_init, yes_step;
            if (c->cycle == 0){
                Clause empty;
                // printf("[proveAndGeneralize] cycle-0:\n");
                if (!init.prove(*c, empty, yes_init, no, c))
                    return false;
                generalizeInit(yes_init);
                yes_step = yes_init;
            }else{
                if (!step.prove(*c, yes_step, no, c))
                    return false;
                
                //check(proveInit(*c, yes_init));
                check(init.prove(*c, yes_step, yes_init));
                assert(subsumes(yes_step, yes_init));
                yes_step = yes_init;
#ifdef GENERALIZE_THEN_PUSH
                generalize(yes_step);
#endif
            }

            if (tip.verbosity >= 4 && c->cycle < yes_step.cycle)
                printf("[proveStep] clause was proved in the future: %d -> %d\n",
                       c->cycle, yes_step.cycle);

            // Push clause forwards as much as possible:
            while (yes_step.cycle < size()-1){
                Clause d = yes_step;
                d.cycle++;
                if (!step.prove(d, yes_step))
                    break;
                check(init.prove(d, yes_step, yes_init));
                assert(subsumes(yes_step, yes_init));
                yes_step = yes_init;
            }

#ifndef GENERALIZE_THEN_PUSH
            if (yes_step.cycle > 0)
                generalize(yes_step);
#endif

            yes = yes_step;
            // TODO: assert something based on subsumtion instead.
            // assert(proveInit(yes, yes_step));
            return true;
        }


        bool Trip::proveStep(const Clause& c, Clause& yes)
        {
            // FIXME: code duplication ...
            Clause yes_init, yes_step;

            if (!step.prove(c, yes_step))
                return false;

            if (tip.verbosity >= 4 && c.cycle < yes_step.cycle)
                printf("[proveStep] clause was proved in the future: %d -> %d\n",
                       c.cycle, yes_step.cycle);

            //check(proveInit(c, yes_init));
            check(init.prove(c, yes_step, yes_init));

            // Calculate union of the two strengthened clauses:
            yes = yes_init + yes_step;
            return true;
        }


        lbool Trip::proveProp(Sig p, SharedRef<ScheduledClause>& no){ return prop.prove(p, no, size()-1); }


        bool Trip::baseCase()
        {
            // Run BMC for the necessary number of initial cycles:
            tip.bmc(0,2);

            // Check if all properties are resolved:
            for (SafeProp p = 0; p < tip.safe_props.size(); p++)
                if (tip.safe_props[p].stat == pstat_Unknown)
                    return false;

            for (LiveProp p = 0; p < tip.live_props.size(); p++)
                if (tip.live_props[p].stat == pstat_Unknown)
                    return false;

            return true;
        }


        void Trip::extractTrace(SharedRef<ScheduledClause> sc, vec<vec<lbool> >& frames)
        {
            // Extract trace:
            for (SharedRef<ScheduledClause> scan = sc; scan != NULL; scan = scan->next){
                // printf("[extractTrace] scan = %p, cycle = %d\n", scan, scan->cycle);
                frames.push();
                for (unsigned i = 0; i < scan->inputs.size(); i++)
                    frames.last().push(scan->inputs[i]);
            }
            // Note: should free memory implicitly using reference counting etc.
            clause_queue.clear();
        }


        void Trip::extendLiveness(LiveProp p)
        {
            assert(tip.live_props[p].sigs.size() == 1);

            Sig evt = tip.live_props[p].sigs[0];
            Sig flp = tip.main.mkInp();
            Sig out = tip.main.mkMux(evt, event_cnts[p].x, flp);

            tip.flps.define(gate(flp), out);

            init.extendLiveness();
            prop.extendLiveness(evt, gate(flp), gate(event_cnts[p].x), out);
            step.extendLiveness(evt, gate(flp), gate(event_cnts[p].x), out);

            num_occ.growTo(tip.main.lastGate(), 0);

            // Add that the new target can not be falsified up to the current cycle:
            vec<Sig> cls;
            cls.push(~flp);
            Clause f(cls, size()-1);
            addClause(f);

            // Add that the new target implies the old target always:
            cls.clear();
            cls.push(~flp);
            cls.push(event_cnts[p].x);
            f = Clause(cls, cycle_Undef);
            addClause(f);

            event_cnts[p].x = flp;
            event_cnts[p].k++;
        }


        void Trip::enqueueClause(SharedRef<ScheduledClause> sc)
        {
            clause_queue.growTo(sc->cycle+1);
            clause_queue[sc->cycle].push(sc);
        }


        SharedRef<ScheduledClause> Trip::getMinClause()
        {
            // Naive implementation:
            int i;
            for (i = 0; i < clause_queue.size() && clause_queue[i].size() == 0; i++)
                ;

            if (i == clause_queue.size())
                return NULL;

            SharedRef<ScheduledClause> result = clause_queue[i].last();
            clause_queue[i].pop();

            return result;
        }


        void Trip::clearInactive()
        {
            // Remove all references to inactive clauses:
            for (GateIt git = tip.main.begin(); git != tip.main.end(); ++git)
                for (unsigned s = 0; s < 2; s++)
                    for (unsigned bwd = 0; bwd < 2; bwd++){
                        Sig x = mkSig(*git, s);
                        SMap<vec<Clause*> >& occ = bwd ? bwd_occurs : fwd_occurs;
                        if (!occ.has(x))
                            continue;

                        int i,j;
                        for (i = j = 0; i < occ[x].size(); i++)
                            if (occ[x][i]->isActive())
                                occ[x][j++] = occ[x][i];
                        occ[x].shrink(i - j);
                    }
            
            // Delete inactive clauses:
            int n_removed = 0;
            for (int k = 0; k < F.size(); k++){
                int i,j;
                for (i = j = 0; i < F[k].size(); i++)
                    if (F[k][i]->isActive())
                        F[k][j++] = F[k][i];
                    else
                        n_removed++, delete F[k][i];
                F[k].shrink(i - j);
            }
            int i,j;
            for (i = j = 0; i < F_inv.size(); i++)
                if (F_inv[i]->isActive())
                    F_inv[j++] = F_inv[i];
                else
                    n_removed++, delete F_inv[i];
            F_inv.shrink(i - j);

            if (tip.verbosity >= 4)
                printf("[clearInactive] removed %d inactive clauses\n", n_removed);
        }


        bool Trip::removeClause(Clause* c)
        {
            // It was not already removed:
            assert(c->isActive());

            c->deactivate();

            if (c->cycle == cycle_Undef){
                n_inv--;
                return false;
            }
            n_total--;
            step.resetCycle(c->cycle, F_size[c->cycle]);

            // Decrease occurrence counts:
            for (unsigned i = 0; i < c->size(); i++){
                assert(num_occ.has(gate((*c)[i])));
                num_occ[gate((*c)[i])]--;
                assert(num_occ[gate((*c)[i])] >= 0);
            }

            // While removing from the last cycle, the set can not become empty:
            assert(c->cycle < (unsigned)size());

            // FIXME: what to check here instead?
            // assert(c->cycle < size()-1 || F_size[c->cycle] > 1);

            F_size[c->cycle]--;
            c->deactivate();

            // FIXME: what to check here instead?
            // Assume that there was no empty set before:
            // assert(F_size[c->cycle] > 0 || F_size[c->cycle+1] > 0);

            // Note: The last cycle does not imply any of the properties and thus if this becomes
            // empty it does NOT mean that the algorithm terminates. Also no further clauses can be
            // extracted as there are no clauses "to the right" of this cycle.
            return c->cycle < size()-1 && F_size[c->cycle] == 0;
        }

        unsigned Trip::size () const { assert(F.size() == F_size.size()); return F.size(); }

        bool Trip::addClause(const Clause& c_)
        {
            unsigned cycle = c_.cycle;
            if (c_.cycle != cycle_Undef){
                assert(cycle <= size());
                // Clause that hold up-to and including 'cycle':
                // FIXME: how to handle the case when 'cycle == size()'
                if (cycle == size())
                    cycle--;
                F[cycle].push(new Clause(c_));
                F_size[cycle]++;
                F[cycle].last()->cycle = cycle;
            }else{
                // Invariant clause:
                F_inv.push(new Clause(c_));
                n_inv++;
            }
            Clause& c = cycle != cycle_Undef ? *F[cycle].last() : *F_inv.last();
            assert(c.size() > 0);

             // printf("[addClause] c = ");
             // printClause(c);
             // printf("\n");

            assert(!fwdSubsumed(&c_));
            n_total++;
            cls_added++;

            // Increase occurrence counts:
            for (unsigned i = 0; i < c.size(); i++)
                num_occ[gate(c[i])]++;

            prop.addClause(c);
            step.addClause(c);

            // Attach to backward subsumption index:
            for (unsigned i = 0; i < c.size(); i++){
                bwd_occurs.growTo(c[i]);
                bwd_occurs[c[i]].push(&c);
            }

            // Attach to forward subsumption index:
            int min_index = 0;
            int min_size  = fwd_occurs.has(c[0]) ? fwd_occurs[c[0]].size() : 0;
            for (unsigned i = 1; i < c.size(); i++){
                Sig x    = c[i];
                int size = fwd_occurs.has(x) ? fwd_occurs[x].size() : 0;
                if (size < min_size){
                    min_size  = size;
                    min_index = i;
                }
            }
            fwd_occurs.growTo(c[min_index]);
            fwd_occurs[c[min_index]].push(&c);

            return bwdSubsume(&c);
        }


        // FIXME: it can happen that the last cycle becoming empty triggers this, and no invariant
        // can be extracted. Should we just let that happen maybe? It does not seem like an error.

        // There is some cycles i,j such that i < j, F[i] == 0 and F[j] > 0. Then the invariant is
        // equal to the union of F[l] for l such that i < l < size().
        void Trip::extractInvariant()
        {
            Clause c;
            for (int i = 0; i < F.size(); i++)
                if (F_size[i] == 0){
                    assert((unsigned)i < size()-1);
                    int inv_size = 0;
                    for (unsigned j = i+1; j < size(); j++)
                        for (int k = 0; k < F[j].size(); k++)
                            if (F[j][k]->isActive()){
                                c = *F[j][k];
                                c.cycle = cycle_Undef;
                                addClause(c);
                                cls_bwdsub--; // Don't count this as a new clause.
                                cls_added--;
                                inv_size++;
                            }
                    if (tip.verbosity >= 3)
                        printf("[extractInvariant] extracted invariant of size %d\n", inv_size);

                    return;
                }

            // Check that some F[i] was empty:
            assert(false);
        }
        

        // PRECONDITION: (incomplete?) 'c' must not already exist in the forward subsumption index.
        bool Trip::fwdSubsumed(const Clause* c, unsigned& cycle)
        {
            unsigned max_subsume_cycle = 0;
            bool     is_subsumed       = false;
            for (unsigned i = 0; i < c->size(); i++){
                Sig x = (*c)[i];
                if (!fwd_occurs.has(x))
                    continue;

                for (int j = 0; j < fwd_occurs[x].size(); j++)
                    if (fwd_occurs[x][j]->isActive() && subsumes(*fwd_occurs[x][j], *c)){
                        cands_fwdsub++;
                        if (max_subsume_cycle < fwd_occurs[x][j]->cycle)
                            max_subsume_cycle = fwd_occurs[x][j]->cycle;
                        is_subsumed = true;
                    }
            }
            if (is_subsumed)
                cycle = max_subsume_cycle;
            return is_subsumed;
        }


        bool Trip::fwdSubsumed(const Clause* c)
        {
            unsigned apa;
            return fwdSubsumed(c, apa);
        }

        
        void Trip::verifySubsumption()
        {
            for (int i = 0; i < F.size(); i++)
                for (int j = 0; j < F[i].size(); j++)
                    if (F[i][j]->isActive())
                        bwdSubsume(F[i][j], true);
        }


        bool Trip::bwdSubsume(Clause* c, bool verify)
        {
            assert(bwd_occurs.has((*c)[0]));
            int min_index = 0;
            int min_size  = bwd_occurs[(*c)[0]].size();
            for (unsigned i = 1; i < c->size(); i++){
                assert(bwd_occurs.has((*c)[i]));
                Sig x    = (*c)[i];
                int size = bwd_occurs[x].size();
                if (size < min_size){
                    min_size  = size;
                    min_index = i;
                }
            }

            const vec<Clause*>& occ = bwd_occurs[(*c)[min_index]];

            bool inv_found = false;
            for (int i = 0; i < occ.size(); i++)
                if (occ[i] != c && occ[i]->isActive() && subsumes(*c, *occ[i])){
                    cls_bwdsub++;
                    if (removeClause(occ[i])){
                        if (verify){
                            printf("[bwdSubsume] spurious subsumption\n");
                            printf("[bwdSubsume] c = ");
                            printClause(*c);
                            printf("\n");
                            printf("[bwdSubsume] d = ");
                            printClause(*occ[i]);
                            printf("\n");
                            assert(false);
                        }
                        inv_found = true;
                    }
                    occ[i]->cycle = c->cycle;
                }

            return inv_found;
        }


        void Trip::printInvariant()
        {
            for (int i = 0; i < F_inv.size(); i++)
                if (F_inv[i]->isActive()){
                    const Clause& c = *F_inv[i];
                    printf(" >> ");
                    printClause(c);
                    printf("\n");
                }
        }


        void Trip::verifyInvariant()
        {
            double time_before = cpuTime();
            clearInactive();

            Solver             s;
            Clausifyer<Solver> cl0(tip.main, s), cl1(tip.main, s);
            Clausifyer<Solver> cli(tip.init, s);

            // Clausify-connect two cycles of the circuit:
            for (int i = 0; i < tip.flps.size(); i++)
                cl1.clausifyAs(tip.flps[i], cl0.clausify(tip.flps.next(tip.flps[i])));

            // Add all clauses to cycle 0:
            for (int i = 0; i < F_inv.size(); i++){
                assert(F_inv[i]->isActive());

                const Clause& c = *F_inv[i];
                vec<Lit>      cs;

                for (unsigned j = 0; j < c.size(); j++)
                    cs.push(cl0.clausify(c[j]));

                s.addClause(cs);
            }

            // Assert constraints in both cycles 0 and 1:
            for (int k = 0; k < 2; k++){
                Clausifyer<Solver>& cl = k == 0 ? cl0 : cl1;
                for (unsigned i = 0; i < tip.cnstrs.size(); i++){
                    Lit x = cl.clausify(tip.cnstrs[i][0]);
                    for (int j = 1; j < tip.cnstrs[i].size(); j++){
                        Lit y = cl.clausify(tip.cnstrs[i][j]);
                        s.addClause(~x, y);
                        s.addClause(~y, x);
                    }
                }
            }

            // Verify that clauses are invariant:
            int num_failed = 0;
            for (int i = 0; i < F_inv.size(); i++){
                assert(F_inv[i]->isActive());

                const Clause& c = *F_inv[i];
                vec<Lit>      cs;
                for (unsigned j = 0; j < c.size(); j++)
                    cs.push(~cl1.clausify(c[j]));

                if (s.solve(cs))
                    num_failed++;
            }

            if (num_failed > 0){
                printf("WARNING! %d clauses are not implied by the candidate invariant.\n", num_failed);
                exit(211); }
            //printf("[verifyInvariant] invariant checked (step) cpu-time = %.2f s\n", cpuTime() - time_before);

            // Verify that properties are implied by the invariant:
            num_failed = 0;
            for (SafeProp p = 0; p < tip.safe_props.size(); p++)
                if (tip.safe_props[p].stat == pstat_Proved)
                    if (s.solve(~cl1.clausify(tip.safe_props[p].sig)))
                        num_failed++;

            // TODO: also check liveness

            if (num_failed > 0){
                printf("WARNING! %d properties are not implied by the candidate invariant.\n", num_failed);
                exit(212); }
            //printf("[verifyInvariant] properties checked cpu-time = %.2f s\n", cpuTime() - time_before);

            // Clausify-connect cycle 0 to the initial circuit:
            for (int i = 0; i < tip.flps.size(); i++)
                cli.clausifyAs(tip.flps.init(tip.flps[i]), cl0.clausify(tip.flps[i]));

            // Verify that clauses hold in cycle 1:
            num_failed = 0;
            for (int i = 0; i < F_inv.size(); i++){
                assert(F_inv[i]->isActive());

                const Clause& c = *F_inv[i];
                vec<Lit>      cs;
                for (unsigned j = 0; j < c.size(); j++)
                    cs.push(~cl0.clausify(c[j]));

                if (s.solve(cs))
                    num_failed++;
            }
            // printf("[verifyInvariant] invariant checked (base) cpu-time = %.2f s\n", cpuTime() - time_before);

            if (num_failed > 0){
                printf("WARNING! %d clauses not true in cycle 1.\n", num_failed);
                exit(213);
            }

            printf("\n");
            printf("INVARIANT VERIFIED (cpu-time %.2f s)\n", cpuTime() - time_before);
            printf("\n");
        }


        void Trip::pushClauses()
        {
            Clause c,d;
            assert(F.size() > 0);

            // clearInactive();

#ifdef VERIFY_SUBSUMPTION
            // Check that no subsumptions were missed.
            verifySubsumption();
#endif
            
            for (int k = 0; k < F.size()-1; k++){
                int i,j;
                unsigned cycle;
                for (i = j = 0; i < F[k].size(); i++){
                    if (bwd_revive && !F[k][i]->isActive()){
                        cycle = F[k][i]->cycle;

                        if (cycle == (unsigned)k){
                            fwdSubsumed(F[k][i], cycle);
                            F[k][i]->cycle = cycle;
                        }

                        if (cycle != cycle_Undef && cycle > (unsigned)k){
                            F[cycle].push(F[k][i]);
                            continue;
                        }
                    }else
                        F[k][j++] = F[k][i];

                    if ((F[k][i]->isActive() || (bwd_revive && F[k][i]->cycle != cycle_Undef))){
                        assert(F[k][i]->cycle == (unsigned)k);

                        c = *F[k][i];
                        c.cycle++;

                        if (proveStep(c, d)){
                            // NOTE: the clause F[c][i] will be removed by backward subsumption.
                            if (!c.isActive()){
                                cls_revived++;
                            }else{
                                cls_moved++;
                                cls_bwdsub--; // Don't count this as a new clause.
                                cls_added--;
                            }

                            if (addClause(d))
                                extractInvariant();
                        }
                    }
                }
                F[k].shrink(i - j);
            }

            clearInactive();
        }


        static bool findClause(SharedRef<ScheduledClause> x, SharedRef<ScheduledClause> xs)
        {
            return xs->next != NULL && (xs->next == x || findClause(x, xs->next));
        }


        bool Trip::proveRec(SharedRef<ScheduledClause> sc, SharedRef<ScheduledClause>& pred)
        {
            uint32_t bound = (restart_luby ? luby(2, luby_index) : 1) * restart_ival;

            enqueueClause(sc);
            for (;;){
                SharedRef<ScheduledClause> sc = getMinClause();

                if (sc == NULL)
                    break;

                unsigned sub_cycle;
                if (fwdSubsumed(&(const Clause&)*sc, sub_cycle)){
                    if (fwd_revive && sub_cycle != cycle_Undef && sub_cycle+1 < size()){
                        cands_revived++;
                        assert(sub_cycle >= sc->cycle);
                        sc->cycle = sub_cycle+1;
                        enqueueClause(sc); }
                    continue;
                }

                Clause minimized;
                static unsigned iters = 0;

                if (proveAndGeneralize(sc, minimized, pred)){
                    if ((iters++ % 10) == 0) printStats(sc->cycle, false);

                    cls_total_size    += minimized.size();
                    cls_total_before  += sc->size();
                    cls_total_removed += sc->size() - minimized.size();

                    if (bound > 0)
                        // Handle restarts:
                        if (restart_cnt == bound){
                            // printf("[proveRec] restart (bound = %d)\n", bound);
                            luby_index++;
                            restart_cnt = 0;
                            clause_queue.clear();
                            return true;
                        }else
                            restart_cnt++;
                    
                    if (addClause(minimized)){
                        extractInvariant();
                    }else if (fwd_inst && minimized.cycle != cycle_Undef && minimized.cycle+1 < size()){
                        sc->cycle = minimized.cycle+1;
                        enqueueClause(sc);
                    }
                }else if (sc->cycle == 0)
                    return false;
                else{
                    cands_added++;
                    cands_total_size    += pred->size();
                    cands_total_removed += tip.flps.size() - pred->size();
                    
                    enqueueClause(pred);
                    enqueueClause(sc);
                }
            }

            if (!restart_luby)
                // If we are using constant restarts, reset the restart counter:
                restart_cnt = 0;

            return true;
        }


        bool Trip::decideCycle()
        {
            double                     time_before = cpuTime();
            SharedRef<ScheduledClause> pred;
            SharedRef<ScheduledClause> start;
            int                        unresolved = 0;

            // Process safety properties:
            for (SafeProp p = 0; p < tip.safe_props.size(); p++)
                if (tip.safe_props[p].stat == pstat_Unknown){
                    lbool prop_res = l_False;
                    do {
                        // printf("[decideCycle] checking safety property %d in cycle %d\n", p, size());
                        prop_res = proveProp(tip.safe_props[p].sig, pred);
                        if (prop_res == l_False){
                            cands_added++;
                            cands_total_size    += pred->size();
                            cands_total_removed += tip.flps.size() - pred->size();
                            if (!proveRec(pred, start)){
                                // 'p' was falsified.
                                printf("[decideCycle] safety property %d was falsified!\n", p);
                                Trace             cex    = tip.newTrace();
                                vec<vec<lbool> >& frames = tip.traces[cex].frames;
                                extractTrace(start, frames);
                                tip.adaptTrace(frames);
                                tip.safe_props[p].stat   = pstat_Falsified;
                                tip.safe_props[p].cex    = cex;
                                break;
                            }
                        }else if (prop_res == l_True){
                            // 'p' is implied by the invariants.
                            tip.safe_props[p].stat = pstat_Proved;
                            printf("[decideCycle] safety property %d was proved!\n", p);
                        }else if (prop_res == l_Undef){
                            // Done with 'p' for this cycle:
                            unresolved++;
                        }
                    }while (prop_res == l_False);
                }

            // Process liveness properties:
            event_cnts.growTo(tip.live_props.size());
            for (LiveProp p = 0; p < tip.live_props.size(); p++)
                if (tip.live_props[p].stat == pstat_Unknown){
                    lbool prop_res = l_False;
                    do {
                        // printf("[decideCycle] checking liveness property %d in cycle %d\n", p, size());
                        prop_res = proveProp(~event_cnts[p].x, pred);
                        if (prop_res == l_False){
                            cands_added++;
                            cands_total_size    += pred->size();
                            cands_total_removed += tip.flps.size() - pred->size();
                            if (!proveRec(pred, start)){
                                // 'p' was falsified.
                                printf("[decideCycle] event counter for liveness property %d reached %d\n", p, event_cnts[p].k);

                                // vec<vec<lbool> > frames;
                                // extractTrace(start, frames);
                                // tip.printTrace(stdout, frames);

                                // Important:
                                clause_queue.clear();

                                extendLiveness(p);
                            }
                        }else if (prop_res == l_True){
                            // 'p' is implied by the invariants.
                            tip.live_props[p].stat = pstat_Proved;
                            printf("[decideCycle] liveness property %d was proved with k=%d!\n", p, event_cnts[p].k);
                        }else if (prop_res == l_Undef){
                            // Done with 'p' for this cycle:
                            unresolved++;
                        }
                    }while (prop_res == l_False);
                }

            bool result;
            // Check if all properties were resolved:
            if (unresolved == 0)
                result = true;
            else{
                // At this point we know that all remaining properties are implied in cycle k+1. Expand
                // a new frame and push clauses forward as much as possible:
                F.push();
                F_size.push(0);
                prop.clearClauses();
                pushClauses();
                result = false;
            }

            cpu_time += cpuTime() - time_before;
            return result;
        }

        uint64_t Trip::props()
        {
            return init.props() + prop.props() + step.props();
        }

        uint64_t Trip::solves(){ return init.solves() + prop.solves() + step.solves(); }


        double   Trip::time()
        {
            return cpu_time;
        }

        int Trip::depth() const { return size()+1; }


        void Trip::printStats(unsigned curr_cycle, bool newline)
        {
            if (tip.verbosity >= 2 || (newline && tip.verbosity >= 1)){
                // Calculate unknown properties:
                unsigned n_safes = 0, n_lives = 0;
                for (SafeProp p = 0; p < tip.safe_props.size(); p++)
                    if (tip.safe_props[p].stat == pstat_Unknown)
                        n_safes++;
                for (LiveProp p = 0; p < tip.live_props.size(); p++)
                    if (tip.live_props[p].stat == pstat_Unknown)
                        n_lives++;

                printf("[rip] ");
                printf("%d:", size());
                for (int i = 0; i < F.size(); i++){
                    printf("%c%d", i == (int)curr_cycle ? '*' : ' ', F_size[i]);
                }
                printf(" (%d) = %d, time = %.1f s", n_inv, n_total, cpu_time);
                printf(", #safes=%d, #lives=%d", n_safes, n_lives);
                printf(newline || tip.verbosity >= 3 ? "\n" : "\r");
                fflush(stdout);
            }
            
            if (tip.verbosity >= 3){
                printf("[rip-stats] #clauses=%d, depth=%d\n", n_total, size());
                init.printStats();
                prop.printStats();
                step.printStats();
            }
        }

        void Trip::printFinalStats()
        {
            printf("Rip statistics:\n");
            printf("================================================================================\n");
            printf("\n");
            printf("Clauses:\n");
            printf("  Added:             %"PRIu64"\n", cls_added);
            printf("  Backward subsumed: %"PRIu64"\n", cls_bwdsub);
            printf("  Moved:             %"PRIu64"\n", cls_moved);
            printf("  Revived:           %"PRIu64"\n", cls_revived);
            printf("  Avg. size:         %.1f\n", cls_total_size / (double)cls_added);
            printf("  Total Literals :   %"PRIu64" (%.1f%% deleted)\n", 
                   cls_total_size, cls_total_removed * 100 / (double)cls_total_before);
            printf("\n");
            printf("\n");

            printf("Candidate Clauses:\n");
            printf("  Added:             %"PRIu64"\n", cands_added);
            printf("  Forward subsumed:  %"PRIu64"\n", cands_fwdsub);
            printf("  Revived:           %"PRIu64"\n", cands_revived);
            printf("  Avg. size:         %.1f\n", cands_total_size / (double)cands_added);
            printf("  Total Literals :   %"PRIu64" (%.1f%% deleted)\n", 
                   cands_total_size, cands_total_removed * 100 / (double)(tip.flps.size() * cands_added));
            printf("\n");

            printf("SAT-solving:      Init-Instance  Step-Instance Prop-Instance\n");
            printf("  Solves:          %12"PRIu64"   %12"PRIu64"  %12"PRIu64"\n",
                   init.solves(), step.solves(), prop.solves());
            printf("  Propagations:    %12"PRIu64"   %12"PRIu64"  %12"PRIu64"\n",
                   init.props(), step.props(), prop.props());
            printf("  CPU-Time:        %12.1f s %12.1f s %12.1f s\n",
                   init.time(), step.time(), prop.time());
        }


        void Trip::printClause(const Clause& c)
        {
            Tip::printClause(tip, c);
        }
    };


    void relativeInduction(TipCirc& tip, RipBmcMode bmc_mode)
    {
        double    time_before = cpuTime();
        Trip      trip(tip);
        BasicBmc* bmc = new BasicBmc(tip);

        // Necessary BMC for relative induction to be sound:
        // TODO: shrink the number of cycles since the initial instance doesn't unroll?
        for (int i = 0; !bmc->done() && i < 2; i++){
            bmc->unrollCycle();
            bmc->decideCycle();
            bmc->printStats ();
        }

        // Take a few cheap extra BMC cycles:
        if (bmc_mode == ripbmc_Safe)
            for (int i = 0; !bmc->done() && bmc->depth() < 30 && bmc->props() < 2000000; i++){
                //printf("[relativeInduction] bmc->props() = %d\n", (int)bmc->props());
                bmc->unrollCycle();
                bmc->decideCycle();
                bmc->printStats ();
            }

        // TODO: implement a clear/reset method in bmc-class instead.
        double bmc_time = 0;
        if (bmc_mode != ripbmc_Live){
            bmc_time = bmc->time();
            delete bmc;
            bmc = NULL;
        }

        while (!trip.decideCycle()){
            trip.printStats();

            // TODO: work on better heuristics here.
            if (bmc_mode == ripbmc_Live)
                while (!bmc->done() && ((bmc->depth() < trip.depth() * 0.5) || 
                                       ((bmc->depth() < trip.depth()*8) && (bmc->props() < trip.props() * 0.5))
                                       )
                       ){
                    bmc->unrollCycle();
                    bmc->decideCycle();
                    bmc->printStats ();
                }
        }

        // If some property was proved, print the invariant:
        for (SafeProp p = 0; p < tip.safe_props.size(); p++)
            if (tip.safe_props[p].stat == pstat_Proved){
#ifdef VERIFY_INVARIANT
                trip.verifyInvariant();
#endif
                if (tip.verbosity >= 3){
                    printf("[relativeInduction] invariant:\n");
                    trip.printInvariant(); }
                break;
            }
        // TODO: also check liveness

        double total_time = cpuTime() - time_before;
        trip.printFinalStats();
        printf("\n");
        printf("CPU-time:\n");
        printf("  Rip:   %.2f s\n", trip.time());
        printf("  Bmc:   %.2f s\n", (bmc != NULL) ? bmc->time() : bmc_time);
        printf("  Total: %.2f s\n", total_time);
        printf("\n");

        if (bmc != NULL)
            delete bmc;
    }


//=================================================================================================
} // namespace Tip
