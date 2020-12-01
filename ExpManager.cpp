// ***************************************************************************************************************
//
//          Mini-Aevol is a reduced version of Aevol -- An in silico experimental evolution platform
//
// ***************************************************************************************************************
//
// Copyright: See the AUTHORS file provided with the package or <https://gitlab.inria.fr/rouzaudc/mini-aevol>
// Web: https://gitlab.inria.fr/rouzaudc/mini-aevol
// E-mail: See <jonathan.rouzaud-cornabas@inria.fr>
// Original Authors : Jonathan Rouzaud-Cornabas
//
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// ***************************************************************************************************************


#include <cmath>
#include <map>
#include <sys/stat.h>
#include <err.h>
#include <chrono>
#include <iostream>

#ifdef USE_CUDA
#include "nvToolsExt.h"
#include <cuda_profiler_api.h>
using namespace std::chrono;

#include "Algorithms.h"
#endif

using namespace std;

#include "ExpManager.h"
#include "AeTime.h"
#include "RNA.h"
#include "Protein.h"
#include "Organism.h"
#include "Gaussian.h"

#include <utility>

/**
 * Constructor for initializing a new simulation
 *
 * @param grid_height : Height of the grid containing the organisms
 * @param grid_width : Width of the grid containing the organisms
 * @param seed : Global seed for all the PRNG of the simulation
 * @param mutation_rate : Mutation rates for all the organism during the simulation
 * @param init_length_dna : Size of the randomly generated DNA at the initialization of the simulation
 * @param selection_pressure : Selection pressure used during the selection process
 * @param backup_step : How much often checkpoint must be done
 */
ExpManager::ExpManager(int grid_height, int grid_width, int seed, double mutation_rate, int init_length_dna,
                       int backup_step)
        : rng_(new Threefry(grid_width, grid_height, seed)) {
    // Initializing the data structure
    grid_height_ = grid_height;
    grid_width_ = grid_width;

    backup_step_ = backup_step;

    nb_indivs_ = grid_height * grid_width;

    internal_organisms_ = new std::shared_ptr<Organism>[nb_indivs_];
    prev_internal_organisms_ = new std::shared_ptr<Organism>[nb_indivs_];

    next_generation_reproducer_ = new int[nb_indivs_]();
    dna_mutator_array_ = new DnaMutator *[nb_indivs_];

    mutation_rate_ = mutation_rate;

    // Building the target environment
    auto *g1 = new Gaussian(1.2, 0.52, 0.12);
    auto *g2 = new Gaussian(-1.4, 0.5, 0.07);
    auto *g3 = new Gaussian(0.3, 0.8, 0.03);

    target = new double[300];
    for (int i = 0; i < 300; i++) {
        double pt_i = ((double) i) / 300.0;

        double tmp = g1->compute_y(pt_i);
        tmp += g2->compute_y(pt_i);
        tmp += g3->compute_y(pt_i);

        tmp = tmp > Y_MAX ? Y_MAX : tmp;
        tmp = tmp < Y_MIN ? Y_MIN : tmp;

        target[i] = tmp;
    }

    delete g1;
    delete g2;
    delete g3;

    geometric_area_ = 0;


    for (int i = 0; i < 299; i++) {
        geometric_area_ += ((fabs(target[i]) + fabs(target[i + 1])) / (600.0));
    }

    printf("Initialized environmental target %f\n", geometric_area_);


    // Initializing the PRNGs
    for (int indiv_id = 0; indiv_id < nb_indivs_; ++indiv_id) {
        dna_mutator_array_[indiv_id] = nullptr;
    }

    // Generate a random organism that is better than nothing
    double r_compare = 0;

    while (r_compare >= 0) {
        auto random_organism = std::make_shared<Organism>(init_length_dna, rng_->gen(0, Threefry::MUTATION));
        internal_organisms_[0] = random_organism;
        start_stop_RNA(0);
        compute_RNA(0);
        start_protein(0);
        compute_protein(0);
        translate_protein(0);
        compute_phenotype(0);
        compute_fitness(0);

        r_compare = round((random_organism->metaerror - geometric_area_) * 1E10) / 1E10;
    }

    printf("Populating the environment\n");

    // Create a population of clones based on the randomly generated organism
    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        prev_internal_organisms_[indiv_id] = internal_organisms_[indiv_id] =
                std::make_shared<Organism>(internal_organisms_[0]);
    }

    // Create backup and stats directory
    create_directory();
}

/**
 * Constructor to resume/restore a simulation from a given backup/checkpoint file
 *
 * @param time : resume from this generation
 */
ExpManager::ExpManager(int time) {
    target = new double[300];

    load(time);

    geometric_area_ = 0;
    for (int i = 0; i < 299; i++) {
        geometric_area_ += ((fabs(target[i]) + fabs(target[i + 1])) / (600.0));
    }

    printf("Initialized environmental target %f\n", geometric_area_);

    dna_mutator_array_ = new DnaMutator *[nb_indivs_];
    for (int indiv_id = 0; indiv_id < nb_indivs_; ++indiv_id) {
        dna_mutator_array_[indiv_id] = nullptr;
    }

}

/**
 * Create stats and backup directory
 */
void ExpManager::create_directory() {
    // Backup
    int status = mkdir("backup", 0755);
    if (status == -1 && errno != EEXIST) {
        err(EXIT_FAILURE, "backup");
    }

    // Stats
    status = mkdir("stats", 0755);
    if (status == -1 && errno != EEXIST) {
        err(EXIT_FAILURE, "stats");
    }
}

/**
 * Checkpointing/Backup of the population of organisms
 *
 * @param t : simulated time of the checkpoint
 */
void ExpManager::save(int t) {

    char exp_backup_file_name[255];

    sprintf(exp_backup_file_name, "backup/backup_%d.zae", t);

    // -------------------------------------------------------------------------
    // Open backup files
    // -------------------------------------------------------------------------
    gzFile exp_backup_file = gzopen(exp_backup_file_name, "w");


    // -------------------------------------------------------------------------
    // Check that files were correctly opened
    // -------------------------------------------------------------------------
    if (exp_backup_file == Z_NULL) {
        printf("Error: could not open backup file %s\n",
               exp_backup_file_name);
        exit(EXIT_FAILURE);
    }


    // -------------------------------------------------------------------------
    // Write the backup file
    // -------------------------------------------------------------------------
    gzwrite(exp_backup_file, &t, sizeof(t));

    gzwrite(exp_backup_file, &grid_height_, sizeof(grid_height_));
    gzwrite(exp_backup_file, &grid_width_, sizeof(grid_width_));

    gzwrite(exp_backup_file, &nb_indivs_, sizeof(nb_indivs_));

    gzwrite(exp_backup_file, &backup_step_, sizeof(backup_step_));

    gzwrite(exp_backup_file, &mutation_rate_, sizeof(mutation_rate_));

    for (int i = 0; i < 300; i++) {
        double tmp = target[i];
        gzwrite(exp_backup_file, &tmp, sizeof(tmp));
    }

    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        prev_internal_organisms_[indiv_id]->save(exp_backup_file);
    }

    rng_->save(exp_backup_file);

    if (gzclose(exp_backup_file) != Z_OK) {
        cerr << "Error while closing backup file" << endl;
    }
}

/**
 * Loading a simulation from a checkpoint/backup file
 *
 * @param t : resuming the simulation at this generation
 */
void ExpManager::load(int t) {

    char exp_backup_file_name[255];

    sprintf(exp_backup_file_name, "backup/backup_%d.zae", t);

    // -------------------------------------------------------------------------
    // Open backup files
    // -------------------------------------------------------------------------
    gzFile exp_backup_file = gzopen(exp_backup_file_name, "r");


    // -------------------------------------------------------------------------
    // Check that files were correctly opened
    // -------------------------------------------------------------------------
    if (exp_backup_file == Z_NULL) {
        printf("Error: could not open backup file %s\n",
               exp_backup_file_name);
        exit(EXIT_FAILURE);
    }


    // -------------------------------------------------------------------------
    // Write the backup file
    // -------------------------------------------------------------------------
    int time;
    gzread(exp_backup_file, &time, sizeof(time));
    AeTime::set_time(time);

    gzread(exp_backup_file, &grid_height_, sizeof(grid_height_));

    gzread(exp_backup_file, &grid_width_, sizeof(grid_width_));

    gzread(exp_backup_file, &nb_indivs_, sizeof(nb_indivs_));

    internal_organisms_ = new std::shared_ptr<Organism>[nb_indivs_];
    prev_internal_organisms_ = new std::shared_ptr<Organism>[nb_indivs_];

    // No need to save/load this field from the backup because it will be set at selection()
    next_generation_reproducer_ = new int[nb_indivs_]();

    gzread(exp_backup_file, &backup_step_, sizeof(backup_step_));

    gzread(exp_backup_file, &mutation_rate_, sizeof(mutation_rate_));

    for (int i = 0; i < 300; i++) {
        double tmp;
        gzread(exp_backup_file, &tmp, sizeof(tmp));
        target[i] = tmp;
    }

    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        prev_internal_organisms_[indiv_id] = internal_organisms_[indiv_id] =
                std::make_shared<Organism>(exp_backup_file);
        // promoters have to be recomputed, they are not save in the backup
        start_stop_RNA(indiv_id);
    }

    rng_ = std::move(std::make_unique<Threefry>(grid_width_, grid_height_, exp_backup_file));

    if (gzclose(exp_backup_file) != Z_OK) {
        cerr << "Error while closing backup file" << endl;
    }
}

/**
 * Prepare the mutation generation of an organism
 *
 * @param indiv_id : Organism unique id
 */
void ExpManager::prepare_mutation(int indiv_id) {
    auto *rng = new Threefry::Gen(std::move(rng_->gen(indiv_id, Threefry::MUTATION)));
    const shared_ptr<Organism> &parent = prev_internal_organisms_[next_generation_reproducer_[indiv_id]];
    dna_mutator_array_[indiv_id] = new DnaMutator(
            rng,
            parent->length(),
            mutation_rate_);
    dna_mutator_array_[indiv_id]->generate_mutations();

    if (dna_mutator_array_[indiv_id]->hasMutate()) {
        internal_organisms_[indiv_id] = std::make_shared<Organism>(parent);
    } else {
        int parent_id = next_generation_reproducer_[indiv_id];

        internal_organisms_[indiv_id] = prev_internal_organisms_[parent_id];
        internal_organisms_[indiv_id]->reset_mutation_stats();
    }
}

/**
 * Destructor of the ExpManager class
 */
ExpManager::~ExpManager() {
    delete stats_best;
    delete stats_mean;

    delete[] dna_mutator_array_;

    delete[] internal_organisms_;
    delete[] prev_internal_organisms_;
    delete[] next_generation_reproducer_;
    delete[] target;
}

/**
 * Execute a generation of the simulation for all the Organisms
 *
 */
void ExpManager::run_a_step() {

    // Running the simulation process for each organism
    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        selection(indiv_id);
        prepare_mutation(indiv_id);

        if (dna_mutator_array_[indiv_id]->hasMutate()) {
            apply_mutation(indiv_id);
            opt_prom_compute_RNA(indiv_id);
            start_protein(indiv_id);
            compute_protein(indiv_id);
            translate_protein(indiv_id);
            compute_phenotype(indiv_id);
            compute_fitness(indiv_id);
        }
    }


    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        prev_internal_organisms_[indiv_id] = internal_organisms_[indiv_id];
        internal_organisms_[indiv_id] = nullptr;
    }

    // Search for the best
    double best_fitness = prev_internal_organisms_[0]->fitness;
    int idx_best = 0;
    for (int indiv_id = 1; indiv_id < nb_indivs_; indiv_id++) {
        if (prev_internal_organisms_[indiv_id]->fitness > best_fitness) {
            idx_best = indiv_id;
            best_fitness = prev_internal_organisms_[indiv_id]->fitness;
        }
    }
    best_indiv = prev_internal_organisms_[idx_best];

    // Stats
    stats_best->reinit(AeTime::time());
    stats_mean->reinit(AeTime::time());

    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        if (dna_mutator_array_[indiv_id]->hasMutate())
            prev_internal_organisms_[indiv_id]->compute_protein_stats();
    }

    stats_best->write_best(best_indiv);
    stats_mean->write_average(prev_internal_organisms_, nb_indivs_);

}


/**
 * Search for Promoters and Terminators (i.e. beginning and ending of a RNA) within the whole DNA of an Organism
 *
 * @param indiv_id : Unique identification number of the organism
 */
void ExpManager::start_stop_RNA(int indiv_id) {
    for (int dna_pos = 0; dna_pos < internal_organisms_[indiv_id]->length(); dna_pos++) {
        if (internal_organisms_[indiv_id]->length() >= PROM_SIZE) {
            int dist_lead = internal_organisms_[indiv_id]->dna_->promoter_at(dna_pos);

            if (dist_lead <= 4) {
                internal_organisms_[indiv_id]->add_new_promoter(dna_pos, dist_lead);
            }

            // Computing if a terminator exists at that position
            int dist_term_lead = internal_organisms_[indiv_id]->dna_->terminator_at(dna_pos);

            if (dist_term_lead == 4) {
                internal_organisms_[indiv_id]->terminators.insert(dna_pos);
            }
        }
    }
}

/**
 * Optimize version that do not need to search the whole Dna for promoters
 */
void ExpManager::opt_prom_compute_RNA(int indiv_id) {
    if (dna_mutator_array_[indiv_id]->hasMutate()) {
        internal_organisms_[indiv_id]->proteins.clear();
        internal_organisms_[indiv_id]->rnas.clear();
        internal_organisms_[indiv_id]->terminators.clear();

        internal_organisms_[indiv_id]->rnas.resize(
                internal_organisms_[indiv_id]->promoters_.size());

        for (const auto &prom_pair: internal_organisms_[indiv_id]->promoters_) {
            int prom_pos = prom_pair.first;

            /* Search for terminators */
            int cur_pos = prom_pos + 22;
            cur_pos = cur_pos >= internal_organisms_[indiv_id]->length()
                      ? cur_pos - internal_organisms_[indiv_id]->length()
                      : cur_pos;

            int start_pos = cur_pos;

            bool terminator_found = false;

            while (!terminator_found) {
                int term_dist_leading = internal_organisms_[indiv_id]->dna_->terminator_at(cur_pos);

                if (term_dist_leading == 4)
                    terminator_found = true;
                else {
                    cur_pos = cur_pos + 1 >= internal_organisms_[indiv_id]->length()
                              ? cur_pos + 1 - internal_organisms_[indiv_id]->length()
                              : cur_pos + 1;

                    if (cur_pos == start_pos) {
                        break;
                    }
                }
            }

            if (terminator_found) {
                int32_t rna_end = cur_pos + 10 >= internal_organisms_[indiv_id]->length()
                                  ? cur_pos + 10 - internal_organisms_[indiv_id]->length()
                                  : cur_pos + 10;

                int32_t rna_length = 0;

                if (prom_pos > rna_end)
                    rna_length = internal_organisms_[indiv_id]->length() - prom_pos + rna_end;
                else
                    rna_length = rna_end - prom_pos;

                rna_length -= 21;

                if (rna_length > 0) {
                    int glob_rna_idx = internal_organisms_[indiv_id]->rna_count_;
                    internal_organisms_[indiv_id]->rna_count_ = internal_organisms_[indiv_id]->rna_count_ + 1;

                    internal_organisms_[indiv_id]->rnas[glob_rna_idx] = new RNA(
                            prom_pos,
                            rna_end,
                            1.0 - std::fabs(
                                    ((float) prom_pair.second)) / 5.0,
                            rna_length);
                }
            }
        }
    }
}


/**
 * Create the list of RNAs based on the found promoters and terminators on the DNA of an Organism
 *
 * @param indiv_id : Unique identification number of the organism
 */
void ExpManager::compute_RNA(int indiv_id) {
    if (internal_organisms_[indiv_id]->terminators.empty())
        return;

    internal_organisms_[indiv_id]->rnas.resize(internal_organisms_[indiv_id]->promoters_.size());

    for (const auto &prom_pair: internal_organisms_[indiv_id]->promoters_) {
        int prom_pos = prom_pair.first;
        int k = prom_pos + 22;

        k = k >= internal_organisms_[indiv_id]->length()
            ? k - internal_organisms_[indiv_id]->length()
            : k;

        auto it_rna_end = internal_organisms_[indiv_id]->terminators.lower_bound(k);

        if (it_rna_end == internal_organisms_[indiv_id]->terminators.end()) {
            it_rna_end = internal_organisms_[indiv_id]->terminators.begin();
        }

        int rna_end = *it_rna_end + 10 >= internal_organisms_[indiv_id]->length()
                      ? *it_rna_end + 10 - internal_organisms_[indiv_id]->length()
                      : *it_rna_end + 10;

        int rna_length = 0;

        if (prom_pos > rna_end)
            rna_length = internal_organisms_[indiv_id]->length() - prom_pos + rna_end;
        else
            rna_length = rna_end - prom_pos;

        rna_length -= 21;

        if (rna_length >= 0) {
            int glob_rna_idx = internal_organisms_[indiv_id]->rna_count_;
            internal_organisms_[indiv_id]->rna_count_ =
                    internal_organisms_[indiv_id]->rna_count_ + 1;

            internal_organisms_[indiv_id]->rnas[glob_rna_idx] =
                    new RNA(prom_pos,
                            rna_end,
                            1.0 - std::fabs(((float) prom_pair.second)) / 5.0,
                            rna_length);
        }
    }
}

/**
 * Search for Shine Dal sequence and Start sequence deliminating the start of genes within one of the RNA of an Organism
 *
 * @param indiv_id : Unique identification number of the organism
 */
void ExpManager::start_protein(int indiv_id) {
    for (int rna_idx = 0; rna_idx < internal_organisms_[indiv_id]->rna_count_; rna_idx++) {
        int c_pos = internal_organisms_[indiv_id]->rnas[rna_idx]->begin;

        if (internal_organisms_[indiv_id]->rnas[rna_idx]->length >= 22) {
            c_pos += 22;
            c_pos = c_pos >= internal_organisms_[indiv_id]->length()
                    ? c_pos - internal_organisms_[indiv_id]->length()
                    : c_pos;

            while (c_pos != internal_organisms_[indiv_id]->rnas[rna_idx]->end) {

                if (internal_organisms_[indiv_id]->dna_->shine_dal_start(c_pos)) {
                    internal_organisms_[indiv_id]->rnas[rna_idx]->start_prot.push_back(c_pos);
                }

                c_pos++;
                c_pos = c_pos >= internal_organisms_[indiv_id]->length()
                        ? c_pos - internal_organisms_[indiv_id]->length()
                        : c_pos;
            }
        }
    }
}

/**
 * Compute the list of genes/proteins of an Organism
 *
 * @param indiv_id : Unique identification number of the organism
 */
void ExpManager::compute_protein(int indiv_id) {
    int resize_to = 0;

    for (int rna_idx = 0; rna_idx < internal_organisms_[indiv_id]->rna_count_; rna_idx++) {
        resize_to += internal_organisms_[indiv_id]->rnas[rna_idx]->start_prot.size();
    }

    internal_organisms_[indiv_id]->proteins.resize(resize_to);

    for (int rna_idx = 0; rna_idx < internal_organisms_[indiv_id]->rna_count_; rna_idx++) {
        for (int protein_idx = 0;
             protein_idx < internal_organisms_[indiv_id]->rnas[rna_idx]->start_prot.size();
             protein_idx++) {

            int protein_start = internal_organisms_[indiv_id]->rnas[rna_idx]->start_prot[protein_idx];
            int current_position = protein_start + 13;

            current_position = current_position >= internal_organisms_[indiv_id]->length()
                               ? current_position - internal_organisms_[indiv_id]->length()
                               : current_position;

            int transcribed_start = internal_organisms_[indiv_id]->rnas[rna_idx]->begin + 22;
            transcribed_start = transcribed_start >= internal_organisms_[indiv_id]->length()
                                ? transcribed_start - internal_organisms_[indiv_id]->length()
                                : transcribed_start;

            int transcription_length;
            if (transcribed_start <= protein_start) {
                transcription_length = protein_start - transcribed_start;
            } else {
                transcription_length = internal_organisms_[indiv_id]->length() - transcribed_start + protein_start;
            }
            transcription_length += 13;


            while (internal_organisms_[indiv_id]->rnas[rna_idx]->length - transcription_length >= 3) {
                if (internal_organisms_[indiv_id]->dna_->protein_stop(current_position)) {
                    int prot_length;

                    int protein_end = current_position + 2 >= internal_organisms_[indiv_id]->length() ?
                                      current_position - internal_organisms_[indiv_id]->length() + 2 :
                                      current_position + 2;

                    if (protein_start + 13 < protein_end) {
                        prot_length = protein_end - (protein_start + 13);
                    } else {
                        prot_length = internal_organisms_[indiv_id]->length() - (protein_start + 13) + protein_end;
                    }

                    if (prot_length >= 3) {
                        int glob_prot_idx = internal_organisms_[indiv_id]->protein_count_;
                        internal_organisms_[indiv_id]->protein_count_ += 1;

                        internal_organisms_[indiv_id]->proteins[glob_prot_idx] =
                                new Protein(protein_start,
                                            protein_end,
                                            prot_length,
                                            internal_organisms_[indiv_id]->rnas[rna_idx]->e);

                        internal_organisms_[indiv_id]->rnas[rna_idx]->is_coding_ = true;
                    }
                    break;
                }

                current_position += 3;
                current_position = current_position >= internal_organisms_[indiv_id]->length()
                                   ? current_position - internal_organisms_[indiv_id]->length()
                                   : current_position;
                transcription_length += 3;
            }
        }
    }
}

/**
 * Compute the pseudo-chimical model (i.e. the width, height and location in the phenotypic space) of a genes/protein
 *
 * @param indiv_id : Unique identification number of the organism
 */
void ExpManager::translate_protein(int indiv_id) {
    for (int protein_idx = 0; protein_idx < internal_organisms_[indiv_id]->protein_count_; protein_idx++) {
        if (internal_organisms_[indiv_id]->proteins[protein_idx]->is_init_) {
            int c_pos = internal_organisms_[indiv_id]->proteins[protein_idx]->protein_start;
            c_pos += 13;
            c_pos = c_pos >= internal_organisms_[indiv_id]->length()
                    ? c_pos - internal_organisms_[indiv_id]->length()
                    : c_pos;

            int codon_list[64] = {};
            int codon_idx = 0;
            int count_loop = 0;

            //printf("Codon list : ");
            while (count_loop < internal_organisms_[indiv_id]->proteins[protein_idx]->protein_length / 3 &&
                   codon_idx < 64) {
                codon_list[codon_idx] = internal_organisms_[indiv_id]->dna_->codon_at(c_pos);
                //printf("%d ",codon_list[codon_idx]);
                codon_idx++;

                count_loop++;
                c_pos += 3;
                c_pos = c_pos >= internal_organisms_[indiv_id]->length()
                        ? c_pos - internal_organisms_[indiv_id]->length()
                        : c_pos;
            }
            //printf("\n");

            double M = 0.0;
            double W = 0.0;
            double H = 0.0;

            int nb_m = 0;
            int nb_w = 0;
            int nb_h = 0;

            bool bin_m = false; // Initializing to false will yield a conservation of the high weight bit
            bool bin_w = false; // when applying the XOR operator for the Gray to standard conversion
            bool bin_h = false;


            for (int i = 0; i < codon_idx; i++) {
                switch (codon_list[i]) {
                    case CODON_M0 : {
                        // M codon found
                        nb_m++;

                        // Convert Gray code to "standard" binary code
                        bin_m ^= false; // as bin_m was initialized to false, the XOR will have no effect on the high weight bit

                        // A lower-than-the-previous-lowest weight bit was found, make a left bitwise shift
                        //~ M <<= 1;
                        M *= 2;

                        // Add this nucleotide's contribution to M
                        if (bin_m) M += 1;

                        break;
                    }
                    case CODON_M1 : {
                        // M codon found
                        nb_m++;

                        // Convert Gray code to "standard" binary code
                        bin_m ^= true; // as bin_m was initialized to false, the XOR will have no effect on the high weight bit

                        // A lower-than-the-previous-lowest bit was found, make a left bitwise shift
                        //~ M <<= 1;
                        M *= 2;

                        // Add this nucleotide's contribution to M
                        if (bin_m) M += 1;

                        break;
                    }
                    case CODON_W0 : {
                        // W codon found
                        nb_w++;

                        // Convert Gray code to "standard" binary code
                        bin_w ^= false; // as bin_m was initialized to false, the XOR will have no effect on the high weight bit

                        // A lower-than-the-previous-lowest weight bit was found, make a left bitwise shift
                        //~ W <<= 1;
                        W *= 2;

                        // Add this nucleotide's contribution to W
                        if (bin_w) W += 1;

                        break;
                    }
                    case CODON_W1 : {
                        // W codon found
                        nb_w++;

                        // Convert Gray code to "standard" binary code
                        bin_w ^= true; // as bin_m was initialized to false, the XOR will have no effect on the high weight bit

                        // A lower-than-the-previous-lowest weight bit was found, make a left bitwise shift
                        //~ W <<= 1;
                        W *= 2;

                        // Add this nucleotide's contribution to W
                        if (bin_w) W += 1;

                        break;
                    }
                    case CODON_H0 :
                    case CODON_START : // Start codon codes for the same amino-acid as H0 codon
                    {
                        // H codon found
                        nb_h++;

                        // Convert Gray code to "standard" binary code
                        bin_h ^= false; // as bin_m was initialized to false, the XOR will have no effect on the high weight bit

                        // A lower-than-the-previous-lowest weight bit was found, make a left bitwise shift
                        //~ H <<= 1;
                        H *= 2;

                        // Add this nucleotide's contribution to H
                        if (bin_h) H += 1;

                        break;
                    }
                    case CODON_H1 : {
                        // H codon found
                        nb_h++;

                        // Convert Gray code to "standard" binary code
                        bin_h ^= true; // as bin_m was initialized to false, the XOR will have no effect on the high weight bit

                        // A lower-than-the-previous-lowest weight bit was found, make a left bitwise shift
                        //~ H <<= 1;
                        H *= 2;

                        // Add this nucleotide's contribution to H
                        if (bin_h) H += 1;

                        break;
                    }
                }
            }

            internal_organisms_[indiv_id]->proteins[protein_idx]->protein_length = codon_idx;


            //  ----------------------------------------------------------------------------------
            //  2) Normalize M, W and H values in [0;1] according to number of codons of each kind
            //  ----------------------------------------------------------------------------------
            internal_organisms_[indiv_id]->proteins[protein_idx]->m =
                    nb_m != 0 ? M / (pow(2, nb_m) - 1) : 0.5;
            internal_organisms_[indiv_id]->proteins[protein_idx]->w =
                    nb_w != 0 ? W / (pow(2, nb_w) - 1) : 0.0;
            internal_organisms_[indiv_id]->proteins[protein_idx]->h =
                    nb_h != 0 ? H / (pow(2, nb_h) - 1) : 0.5;

            //  ------------------------------------------------------------------------------------
            //  3) Normalize M, W and H values according to the allowed ranges (defined in macros.h)
            //  ------------------------------------------------------------------------------------
            // x_min <= M <= x_max
            // w_min <= W <= w_max
            // h_min <= H <= h_max
            internal_organisms_[indiv_id]->proteins[protein_idx]->m =
                    (X_MAX - X_MIN) *
                    internal_organisms_[indiv_id]->proteins[protein_idx]->m +
                    X_MIN;
            internal_organisms_[indiv_id]->proteins[protein_idx]->w =
                    (W_MAX - W_MIN) *
                    internal_organisms_[indiv_id]->proteins[protein_idx]->w +
                    W_MIN;
            internal_organisms_[indiv_id]->proteins[protein_idx]->h =
                    (H_MAX - H_MIN) *
                    internal_organisms_[indiv_id]->proteins[protein_idx]->h +
                    H_MIN;

            if (nb_m == 0 || nb_w == 0 || nb_h == 0 ||
                internal_organisms_[indiv_id]->proteins[protein_idx]->w == 0.0 ||
                internal_organisms_[indiv_id]->proteins[protein_idx]->h == 0.0) {
                internal_organisms_[indiv_id]->proteins[protein_idx]->is_functional = false;
            } else {
                internal_organisms_[indiv_id]->proteins[protein_idx]->is_functional = true;
            }
        }
    }


    std::map<int, Protein *> lookup;

    for (int protein_idx = 0; protein_idx < internal_organisms_[indiv_id]->protein_count_; protein_idx++) {
        if (internal_organisms_[indiv_id]->proteins[protein_idx]->is_init_) {
            if (lookup.find(internal_organisms_[indiv_id]->proteins[protein_idx]->protein_start) == lookup.end()) {
                lookup[internal_organisms_[indiv_id]->proteins[protein_idx]->protein_start] =
                        internal_organisms_[indiv_id]->proteins[protein_idx];
            } else {
                lookup[internal_organisms_[indiv_id]->proteins[protein_idx]->protein_start]->e +=
                        internal_organisms_[indiv_id]->proteins[protein_idx]->e;
                internal_organisms_[indiv_id]->proteins[protein_idx]->is_init_ = false;
            }
        }

    }
}

/**
 * From the list of proteins, build the phenotype of an organism
 *
 * @param indiv_id : Unique identification number of the organism
 */
void ExpManager::compute_phenotype(int indiv_id) {
    double activ_phenotype[300]{};
    double inhib_phenotype[300]{};

    for (int protein_idx = 0; protein_idx < internal_organisms_[indiv_id]->protein_count_; protein_idx++) {
        if (internal_organisms_[indiv_id]->proteins[protein_idx]->is_init_ &&
            fabs(internal_organisms_[indiv_id]->proteins[protein_idx]->w) >= 1e-15 &&
            fabs(internal_organisms_[indiv_id]->proteins[protein_idx]->h) >= 1e-15 &&
            internal_organisms_[indiv_id]->proteins[protein_idx]->is_functional) {
            // Compute triangle points' coordinates
            double x0 =
                    internal_organisms_[indiv_id]->proteins[protein_idx]->m -
                    internal_organisms_[indiv_id]->proteins[protein_idx]->w;

            double x1 = internal_organisms_[indiv_id]->proteins[protein_idx]->m;
            double x2 =
                    internal_organisms_[indiv_id]->proteins[protein_idx]->m +
                    internal_organisms_[indiv_id]->proteins[protein_idx]->w;

            int ix0 = (int) (x0 * 300);
            int ix1 = (int) (x1 * 300);
            int ix2 = (int) (x2 * 300);

            if (ix0 < 0) ix0 = 0; else if (ix0 > (299)) ix0 = 299;
            if (ix1 < 0) ix1 = 0; else if (ix1 > (299)) ix1 = 299;
            if (ix2 < 0) ix2 = 0; else if (ix2 > (299)) ix2 = 299;

            // Compute the first equation of the triangle
            double incY =
                    (internal_organisms_[indiv_id]->proteins[protein_idx]->h *
                     internal_organisms_[indiv_id]->proteins[protein_idx]->e) /
                    (ix1 - ix0);
            int count = 1;

            // Updating value between x0 and x1
            for (int i = ix0 + 1; i < ix1; i++) {
                if (internal_organisms_[indiv_id]->proteins[protein_idx]->h > 0)
                    activ_phenotype[i] += (incY * (count++));
                else
                    inhib_phenotype[i] += (incY * (count++));
            }


            if (internal_organisms_[indiv_id]->proteins[protein_idx]->h > 0)
                activ_phenotype[ix1] += (internal_organisms_[indiv_id]->proteins[protein_idx]->h *
                                         internal_organisms_[indiv_id]->proteins[protein_idx]->e);
            else
                inhib_phenotype[ix1] += (internal_organisms_[indiv_id]->proteins[protein_idx]->h *
                                         internal_organisms_[indiv_id]->proteins[protein_idx]->e);


            // Compute the second equation of the triangle
            incY = (internal_organisms_[indiv_id]->proteins[protein_idx]->h *
                    internal_organisms_[indiv_id]->proteins[protein_idx]->e) /
                   (ix2 - ix1);
            count = 1;

            // Updating value between x1 and x2
            for (int i = ix1 + 1; i < ix2; i++) {
                if (internal_organisms_[indiv_id]->proteins[protein_idx]->h > 0)
                    activ_phenotype[i] += ((internal_organisms_[indiv_id]->proteins[protein_idx]->h *
                                            internal_organisms_[indiv_id]->proteins[protein_idx]->e) -
                                           (incY * (count++)));
                else
                    inhib_phenotype[i] += ((internal_organisms_[indiv_id]->proteins[protein_idx]->h *
                                            internal_organisms_[indiv_id]->proteins[protein_idx]->e) -
                                           (incY * (count++)));
            }
        }
    }


    for (int fuzzy_idx = 0; fuzzy_idx < 300; fuzzy_idx++) {
        if (activ_phenotype[fuzzy_idx] > 1)
            activ_phenotype[fuzzy_idx] = 1;
        if (inhib_phenotype[fuzzy_idx] < -1)
            inhib_phenotype[fuzzy_idx] = -1;
    }

    for (int fuzzy_idx = 0; fuzzy_idx < 300; fuzzy_idx++) {
        internal_organisms_[indiv_id]->phenotype[fuzzy_idx] = activ_phenotype[fuzzy_idx] + inhib_phenotype[fuzzy_idx];
        if (internal_organisms_[indiv_id]->phenotype[fuzzy_idx] < 0)
            internal_organisms_[indiv_id]->phenotype[fuzzy_idx] = 0;
        if (internal_organisms_[indiv_id]->phenotype[fuzzy_idx] > 1)
            internal_organisms_[indiv_id]->phenotype[fuzzy_idx] = 1;
    }
}

/**
 * From the phenotype of an organism, compute its metabolic error and fitness
 *
 * @param indiv_id : Unique identification number of the organism
 * @param selection_pressure : Selection pressure used during the selection process
 */
void ExpManager::compute_fitness(int indiv_id) {
    for (int fuzzy_idx = 0; fuzzy_idx < 300; fuzzy_idx++) {
        internal_organisms_[indiv_id]->delta[fuzzy_idx] = internal_organisms_[indiv_id]->phenotype[fuzzy_idx] -
                                                          target[fuzzy_idx];
    }

    internal_organisms_[indiv_id]->metaerror = 0;

    for (int fuzzy_idx = 0; fuzzy_idx < 299; fuzzy_idx++) {
        internal_organisms_[indiv_id]->metaerror +=
                ((std::fabs(internal_organisms_[indiv_id]->delta[fuzzy_idx]) +
                  std::fabs(internal_organisms_[indiv_id]->delta[fuzzy_idx + 1])) /
                 (600.0));
    }

    internal_organisms_[indiv_id]->fitness =
            exp(-SELECTION_PRESSURE * ((double) internal_organisms_[indiv_id]->metaerror));
}

/**
 * Selection process: for a given cell in the grid of the population, compute which organism win the computation
 *
  * @param indiv_id : Unique identification number of the cell
 */
void ExpManager::selection(int indiv_id) {
    int8_t selection_scope_x = 3;
    int8_t selection_scope_y = 3;
    int8_t neighborhood_size = 9;

    double local_fit_array[neighborhood_size];
    double probs[neighborhood_size];
    int count = 0;
    double sum_local_fit = 0.0;

    int32_t x = indiv_id / grid_height_;
    int32_t y = indiv_id % grid_height_;

    int cur_x, cur_y;

    for (int8_t i = -1; i < selection_scope_x - 1; i++) {
        for (int8_t j = -1; j < selection_scope_y - 1; j++) {
            cur_x = (x + i + grid_width_) % grid_width_;
            cur_y = (y + j + grid_height_) % grid_height_;

            local_fit_array[count] = prev_internal_organisms_[cur_x * grid_height_ + cur_y]->fitness;
            sum_local_fit += local_fit_array[count];

            count++;
        }
    }

    for (int8_t i = 0; i < neighborhood_size; i++) {
        probs[i] = local_fit_array[i] / sum_local_fit;
    }

    auto rng = std::move(rng_->gen(indiv_id, Threefry::REPROD));
    int found_org = rng.roulette_random(probs, neighborhood_size);

    int x_offset = (found_org / selection_scope_x) - 1;
    int y_offset = (found_org % selection_scope_y) - 1;

    next_generation_reproducer_[indiv_id] = ((x + x_offset + grid_width_) % grid_width_) * grid_height_ +
                                            ((y + y_offset + grid_height_) % grid_height_);
}

/**
 * Run the evolution for a given number of generation
 *
 * @param nb_gen : Number of generations to simulate
 */
void ExpManager::run_evolution(int nb_gen) {
    for (int indiv_id = 0; indiv_id < nb_indivs_; indiv_id++) {
        // dna_mutator_array_ is set only to have has_mutate() true so that RNA, protein and phenotype will be computed
        dna_mutator_array_[indiv_id] = new DnaMutator(nullptr, 0, 0);
        dna_mutator_array_[indiv_id]->setMutate(true);

        opt_prom_compute_RNA(indiv_id);
        start_protein(indiv_id);
        compute_protein(indiv_id);
        translate_protein(indiv_id);
        compute_phenotype(indiv_id);
        compute_fitness(indiv_id);
        prev_internal_organisms_[indiv_id]->compute_protein_stats();

        delete dna_mutator_array_[indiv_id];
    }

    // Stats
    stats_best = new Stats(AeTime::time(), true);
    stats_mean = new Stats(AeTime::time(), false);

    printf("Running evolution from %d to %d\n", AeTime::time(), AeTime::time() + nb_gen);

    for (int gen = 0; gen < nb_gen; gen++) {
        AeTime::plusplus();

        run_a_step();

        printf("Generation %d : Best individual fitness %e\n", AeTime::time(), best_indiv->fitness);

        for (int indiv_id = 0; indiv_id < nb_indivs_; ++indiv_id) {
            delete dna_mutator_array_[indiv_id];
            dna_mutator_array_[indiv_id] = nullptr;
        }

        if (AeTime::time() % backup_step_ == 0) {
            save(AeTime::time());
            cout << "Backup for generation " << AeTime::time() << " done !" << endl;
        }
    }
}

#ifdef USE_CUDA
void ExpManager::run_evolution_on_gpu(int nb_gen) {
  cudaProfilerStart();
  high_resolution_clock::time_point t1 = high_resolution_clock::now();
  cout << "Transfer" << endl;
  transfer_in(this, true);
  high_resolution_clock::time_point t2 = high_resolution_clock::now();
  auto duration_transfer_in = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();
  cout << "Transfer done in " << duration_transfer_in << endl;

  printf("Running evolution GPU from %d to %d\n",AeTime::time(),AeTime::time()+nb_gen);
  bool firstGen = true;
  for (int gen = 0; gen < nb_gen+1; gen++) {
    if(gen == 91) nvtxRangePushA("generation 91 to 100");
    AeTime::plusplus();

      high_resolution_clock::time_point t1 = high_resolution_clock::now();
      run_a_step_on_GPU(nb_indivs_, w_max_, selection_pressure_, grid_width_, grid_height_,mutation_rate_);

      t2 = high_resolution_clock::now();
      auto duration_transfer_in = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();

      std::cout<<"LOG,"<<duration_transfer_in<<std::endl;

    firstGen = false;
    if(gen == 100) nvtxRangePop();
    printf("Generation %d : \n",AeTime::time());
  }
  cudaProfilerStop();
}
#endif
