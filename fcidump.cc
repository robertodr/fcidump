/*
 *@BEGIN LICENSE
 *
 * fcidump by James Spencer, a plugin to:
 *
 * PSI4: an ab initio quantum chemistry software package
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *@END LICENSE
 */


#include "psi4/psi4-dec.h"
#include "psi4/libparallel/parallel.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/libmints/mintshelper.h"
#include "psi4/libmints/view.h"
#include "psi4/libpsio/psio.hpp"
#include "psi4/libqt/qt.h"
#include "psi4/libiwl/iwl.hpp"
#include "psi4/libtrans/integraltransform.h"
#include "psi4/libtrans/mospace.h"
#include "psi4/libdpd/dpd.h"
#include "psi4/libciomr/libciomr.h"
#include "psi4/libmints/vector.h"
#include "psi4/libmints/vector3.h"
#include "psi4/libmints/matrix.h"
#include "psi4/libmints/wavefunction.h"
#include "psi4/libmints/molecule.h"
#include "psi4/libmints/dipole.h"
#include "psi4/libmints/quadrupole.h"

#include <vector>

#include "psi4/psifiles.h"

// This allows us to be lazy in getting the spaces in DPD calls
#define ID(x) ints.DPD_ID(x)

// Shamelessly borrow from mrcc.cc.

namespace psi{ namespace fcidump{

typedef int (*orb_indx)(const int);

int mo_index(const int i) {
    // Convert a molecular orbital index [0,1,...] to [1,2,...] (i.e. from zero-based to one-based).
    return i+1;
}

int alpha_index(const int i) {
    // Convert an alpha spin-orbital index [0,1,...] to [1,3,...] (i.e. from
    // zero-based to one-based, with corresponding beta orbitals interwoven).
    return 2*i+1;
}

int beta_index(const int i) {
    // Convert a beta spin-orbital index [0,1,...] to [2,4,...] (i.e. from
    // zero-based to one-based, with corresponding alpha orbitals interwoven).
    return 2*(i+1);
}

void write_oei_to_disk(FILE* intdump, SharedMatrix moH, double ints_tolerance, orb_indx indx)
{
    // Walk through moH and save the non-zero values
    int offset = 0;
    for (int h=0; h<moH->nirrep(); ++h) {
        for (int m=0; m<moH->rowdim(h); ++m) {
            for (int n=0; n<=m; ++n) {
                if (fabs(moH->get(h, m, n)) > ints_tolerance) {
                    fprintf(intdump, "%29.20E%4d%4d%4d%4d\n", moH->get(h, m, n), indx(m+offset), indx(n+offset), 0, 0);
                }
            }
        }
        offset += moH->rowdim(h);
    }
}

void write_tei_to_disk(FILE* intdump, int nirrep, dpdbuf4& K, double ints_tolerance, orb_indx indx1, orb_indx indx2)
{
    for(int h = 0; h < nirrep; ++h){
        global_dpd_->buf4_mat_irrep_init(&K, h);
        global_dpd_->buf4_mat_irrep_rd(&K, h);
        for(int pq = 0; pq < K.params->rowtot[h]; ++pq){
            int p = K.params->roworb[h][pq][0];
            int q = K.params->roworb[h][pq][1];
            for(int rs = 0; rs < K.params->coltot[h]; ++rs){
                int r = K.params->colorb[h][rs][0];
                int s = K.params->colorb[h][rs][1];

                if (fabs(K.matrix[h][pq][rs]) > ints_tolerance)
                    fprintf(intdump, "%28.20E%4d%4d%4d%4d\n",
                            K.matrix[h][pq][rs], indx1(p), indx1(q), indx2(r), indx2(s));
            }
        }
        global_dpd_->buf4_mat_irrep_close(&K, h);
    }
}

void write_eigv_to_disk(FILE* intdump, Dimension frzcpi, Dimension active_mopi, const std::shared_ptr<Vector> eigv, orb_indx indx)
{
    int iorb = 0;
    for (int h=0; h<active_mopi.n(); ++h) {
        for (int i=frzcpi[h]; i<frzcpi[h]+active_mopi[h]; ++i) {
            fprintf(intdump, "%28.20E%4d%4d%4d%4d\n",
                    eigv->get(h,i), indx(iorb), 0, 0, 0);
            iorb++;
        }
    }
}

void write_oei_prop_to_disk(FILE* intdump, std::shared_ptr<Wavefunction> wfn, SharedMatrix prop_ints, double ints_tolerance, orb_indx indx, double *frz_contrib) {

    double** scf = wfn->Ca()->to_block_matrix(); // TODO: UHF
    int nso = wfn->nso();
    int nmo = wfn->nmo();
    Dimension frzcpi      = wfn->frzcpi();
    Dimension active_mopi = wfn->nmopi() - frzcpi - wfn->frzvpi();
    int nirrep = wfn->nirrep();

    double **TMP1 = prop_ints->to_block_matrix();
    double **TMP2 = block_matrix(nso,nso);

    C_DGEMM('n','n',nso,nmo,nso,1,TMP1[0],nso,scf[0],nmo,0,TMP2[0],nso);
    C_DGEMM('t','n',nmo,nmo,nso,1,scf[0],nmo,TMP2[0],nso,0,TMP1[0],nmo);
    // TMP1 now holds the dipole integrals in the MO basis, ordered 1->nmo (in symmetry blocks).
    // We just want to print out the active orbitals...
    // Can't just loop over the two indices as we only know the
    // active orbitals per irrep.  Instead, loop over everything
    // and just print out non-zero integrals (bit slower as we
    // don't use symmetry, but this isn't a hotspot...)
    int ioff1 = 0;
    int nfrz1 = 0;
    for (int h1=0; h1<nirrep; ++h1) {
        nfrz1 += frzcpi[h1];
        int ioff2 = ioff1;
        int nfrz2 = nfrz1;
        for (int h2=h1; h2<nirrep; ++h2) {
            for (int m1=frzcpi[h1]; m1<frzcpi[h1]+active_mopi[h1]; ++m1) {
                int m2_init = h1 == h2 ? m1 : frzcpi[h2];
                for (int m2=m2_init; m2<frzcpi[h2]+active_mopi[h2]; ++m2) {
                    int iorb1 = m1+ioff1;
                    int iorb2 = m2+ioff2;
                    double intgrl = TMP1[iorb1][iorb2];
                    if (fabs(intgrl) > ints_tolerance) fprintf(intdump, "%29.20E%4d%4d\n", intgrl, indx(iorb1-nfrz1), indx(iorb2-nfrz2));
                }
            }
            nfrz2 += frzcpi[h2];
            ioff2 += prop_ints->rowdim(h2);
        }
        ioff1 += prop_ints->rowdim(h1);
    }
    // The contribution of the frozen core orbitals to a one-body
    // expectation value is just \sum_i <i|O_1|i>.
    *frz_contrib = 0.0;
    ioff1 = 0;
    for (int h=0; h<nirrep; ++h) {
        for (int m=0; m<frzcpi[h]; ++m) {
            int iorb = m+ioff1;
            *frz_contrib += 2*TMP1[iorb][iorb]; // 2* for RHF.
        }
        ioff1 += prop_ints->rowdim(h);
    }
}

extern "C" int
read_options(std::string name, Options &options)
{
    if (name == "FCIDUMP" || options.read_globals()) {
        /*- The filename to which all one- and two-electron interals are written, along with information about the single-particle orbitals. -*/
        options.add_str("INTEGRALS_FILE", "INTDUMP");
        /*- Also write out dipole integrals? -*/
        options.add_bool("DIPOLE_INTEGRALS", false);
        /*- Print out single-particle eigenvalues? -*/
        options.add_bool("PRINT_EIGENVALUES", true);
    }

    return true;
}


extern "C" SharedWavefunction
fcidump(SharedWavefunction wfn, Options &options)
{
    if(!wfn) throw PSIEXCEPTION("SCF has not been run yet!");
    std::shared_ptr<Molecule>     molecule = wfn->molecule();
   
    // Grab the global (default) PSIO object, for file I/O
    std::shared_ptr<PSIO> psio(_default_psio_lib_);

    // Orbitals spaces
    Dimension docc        = wfn->doccpi();
    Dimension frzcpi      = wfn->frzcpi();
    Dimension frzvpi      = wfn->frzvpi();
    Dimension active_docc = docc - frzcpi;
    Dimension active_socc = wfn->soccpi();
    Dimension active_mopi = wfn->nmopi() - frzcpi - frzvpi;

    int nbf = active_mopi.sum();
    int nirrep = wfn->nirrep();
    int nelectron = 2 * active_docc.sum() + active_socc.sum();

    // Check the reference.
    bool restricted = true;
    // ..and if we're doing bonus features
    bool dump_dipoles = options.get_bool("DIPOLE_INTEGRALS");

    outfile->Printf("Generating FCIDUMP.\n");
    if (wfn->same_a_b_orbs()) {
        outfile->Printf("Found RHF\n");
    } else {
        outfile->Printf("Found UHF\n");
        restricted = false;
        // write out using spin orbitals rather than molecular orbitals.
        nbf *= 2;
    }

    if (options.get_str("REFERENCE") == "ROHF")
        throw PSIEXCEPTION("FCIDUMP not implemented for ROHF references.");

    FILE* intdump = fopen(options.get_str("INTEGRALS_FILE").c_str(), "w");

    fprintf(intdump, "&FCI\n");
    fprintf(intdump, "NORB=%d,\n", nbf);
    fprintf(intdump, "NELEC=%d,\n", nelectron);
    fprintf(intdump, "MS2=%d,\n", wfn->nalpha()-wfn->nbeta());
    if (restricted) {
        fprintf(intdump, "UHF=.FALSE.,\n");
    } else {
        fprintf(intdump, "UHF=.TRUE.,\n");
    }
    // TODO: is this the correct way to get symmetry information for alpha and beta orbitals?
    fprintf(intdump, "ORBSYM=");
    for (int h=0; h<active_mopi.n(); ++h) {
        for (int n=frzcpi[h]; n<frzcpi[h]+active_mopi[h]; ++n) {
            fprintf(intdump, "%d,", h+1);  // 1 based irrep ordering
            if (!restricted) {
                // need to print out symmetry of beta orbital as well.
                fprintf(intdump, "%d,", h+1);
            }
        }
    }
    fprintf(intdump, "\n&END\n");

    // Define the orbital space of the MO integrals we need.
    std::vector<std::shared_ptr<MOSpace> > spaces;
    spaces.push_back(MOSpace::all);

    // Create integral transformation object
    IntegralTransform ints(wfn, spaces, restricted ? IntegralTransform::Restricted : IntegralTransform::Unrestricted);

    // This transforms everything (OEI and TEI)
    ints.transform_tei(MOSpace::all, MOSpace::all, MOSpace::all, MOSpace::all);

    // Use the IntegralTransform object's DPD instance, for convenience
    dpd_set_default(ints.get_dpd_id());

    outfile->Printf("    Transformation complete.\n");
    outfile->Printf("  Generating fort.55 integral file..\n.");

    double ints_tolerance = options.get_double("INTS_TOLERANCE");

    _default_psio_lib_->open(PSIF_LIBTRANS_DPD, PSIO_OPEN_OLD);
    dpdbuf4 K;

    // RHF
    if (restricted) {

        // We want only the permutationally unique integrals, hence [A>=A]+, see libtrans documenation for details
        global_dpd_->buf4_init(&K, PSIF_LIBTRANS_DPD, 0,
                      ints.DPD_ID("[A>=A]+"), ints.DPD_ID("[A>=A]+"), // In memory
                      ints.DPD_ID("[A>=A]+"), ints.DPD_ID("[A>=A]+"), // On disk
                      0,
                      "MO Ints (AA|AA)");
        write_tei_to_disk(intdump, nirrep, K, ints_tolerance, mo_index, mo_index);
        global_dpd_->buf4_close(&K);

        // Load in frozen core operator, in the event of FREEZE_CORE = FALSE this is the MO OEI
        SharedMatrix moH(new Matrix(PSIF_MO_FZC, wfn->nmopi(), wfn->nmopi()));
        moH->load(_default_psio_lib_, PSIF_OEI);
        View vmoH(moH, active_mopi, active_mopi, frzcpi, frzcpi);
        moH = vmoH();
        write_oei_to_disk(intdump, moH, ints_tolerance, mo_index);

        // Print out single-particle eigenvalues.
        if (options.get_bool("PRINT_EIGENVALUES"))
            write_eigv_to_disk(intdump, frzcpi, active_mopi, wfn->epsilon_a(), mo_index);

        // Print nuclear repulsion energy + frozen core energy.
        fprintf(intdump, "%28.20E%4d%4d%4d%4d\n", ints.get_frozen_core_energy() + molecule->nuclear_repulsion_energy(), 0, 0, 0, 0);

        if (dump_dipoles) {
            // TODO: dipoles filename.
            // TODO: use IntegralTransform.  But, as I don't know how that
            // works, for now follow preppert.cc (part of ccresponse) and do
            // the transformation ourselves.
            std::string fname[] = {"DIPOLES_X", "DIPOLES_Y", "DIPOLES_Z"};
            MintsHelper mints(wfn->basisset(), Process::environment.options, 0);
            std::vector<SharedMatrix> dipole = mints.so_dipole();
            FILE *dipoledump;
            double frz_contrib;
            Vector3 origin = Vector3( 0, 0, 0 ); // In serious trouble if being asked for properties after moving the molecule...
            SharedVector ndip = DipoleInt::nuclear_contribution(molecule, origin);
            for (int i=0; i<3; i++) {
                dipoledump = fopen(fname[i].c_str(), "w");
                write_oei_prop_to_disk(dipoledump, wfn, dipole[i], ints_tolerance, mo_index, &frz_contrib);
                fprintf(dipoledump, "%29.20E%4d%4d\n", ndip->get(i)+frz_contrib, 0, 0);
                fclose(dipoledump);
            }
            // BONUS: and quadrupole moments.  (Just zz for now.)
            std::vector<SharedMatrix> trquad = mints.so_traceless_quadrupole();
            SharedVector nquad = QuadrupoleInt::nuclear_contribution(molecule, origin);
            {
                int ij = 5;
                dipoledump = fopen("TRQUAD_ZZ", "w");
                write_oei_prop_to_disk(dipoledump, wfn, trquad[ij], ints_tolerance, mo_index, &frz_contrib);
                fprintf(dipoledump, "%29.20E%4d%4d\n", nquad->get(0,ij)+frz_contrib, 0, 0);
                fclose(dipoledump);
            }
        }
    }
    else {

        // We want only the permutationally unique integrals, hence [A>=A]+, see libtrans documenation for details

        // Load up alpha alpha
        global_dpd_->buf4_init(&K, PSIF_LIBTRANS_DPD, 0,
                      ints.DPD_ID("[A>=A]+"), ints.DPD_ID("[A>=A]+"), // In memory
                      ints.DPD_ID("[A>=A]+"), ints.DPD_ID("[A>=A]+"), // On disk
                      0,
                      "MO Ints (AA|AA)");
        write_tei_to_disk(intdump, nirrep, K, ints_tolerance, alpha_index, alpha_index);
        global_dpd_->buf4_close(&K);

        // Load up beta beta
        global_dpd_->buf4_init(&K, PSIF_LIBTRANS_DPD, 0,
                      ints.DPD_ID("[a>=a]+"), ints.DPD_ID("[a>=a]+"), // In memory
                      ints.DPD_ID("[a>=a]+"), ints.DPD_ID("[a>=a]+"), // On disk
                      0,
                      "MO Ints (aa|aa)");
        write_tei_to_disk(intdump, nirrep, K, ints_tolerance, beta_index, beta_index);
        global_dpd_->buf4_close(&K);

        // Load up alpha beta
        global_dpd_->buf4_init(&K, PSIF_LIBTRANS_DPD, 0,
                      ints.DPD_ID("[A>=A]+"), ints.DPD_ID("[a>=a]+"), // In memory
                      ints.DPD_ID("[A>=A]+"), ints.DPD_ID("[a>=a]+"), // On disk
                      0,
                      "MO Ints (AA|aa)");
        write_tei_to_disk(intdump, nirrep, K, ints_tolerance, alpha_index, beta_index);
        global_dpd_->buf4_close(&K);

        // Load in alpha frozen core operator, in the event of FREEZE_CORE = FALSE this is the MO OEI
        SharedMatrix moH(new Matrix(PSIF_MO_A_FZC, wfn->nmopi(), wfn->nmopi()));
        moH->load(_default_psio_lib_, PSIF_OEI);
        View vmoH(moH, active_mopi, active_mopi, frzcpi, frzcpi);
        moH = vmoH();
        write_oei_to_disk(intdump, moH, ints_tolerance, alpha_index);

        // Load in beta frozen core operator, in the event of FREEZE_CORE = FALSE this is the MO OEI
        SharedMatrix moHb(new Matrix(PSIF_MO_B_FZC, wfn->nmopi(), wfn->nmopi()));
        moHb->load(_default_psio_lib_, PSIF_OEI);
        View vmoHb(moHb, active_mopi, active_mopi, frzcpi, frzcpi);
        moHb = vmoHb();
        write_oei_to_disk(intdump, moHb, ints_tolerance, beta_index);

        if (options.get_bool("PRINT_EIGENVALUES")) {
            // Print out alpha single-particle eigenvalues.
            write_eigv_to_disk(intdump, frzcpi, active_mopi, wfn->epsilon_a(), alpha_index);
            // Print out beta single-particle eigenvalues.
            write_eigv_to_disk(intdump, frzcpi, active_mopi, wfn->epsilon_b(), beta_index);
        }

        // Print nuclear repulsion energy + frozen core energy.
        fprintf(intdump, "%28.20E%4d%4d%4d%4d\n", ints.get_frozen_core_energy() + molecule->nuclear_repulsion_energy(), 0, 0, 0, 0);
    }
    _default_psio_lib_->close(PSIF_LIBTRANS_DPD, 1);

    fclose(intdump);
    outfile->Printf("Done generating FCIDUMP.\n");

    return wfn;
}

}} // End Namespaces
