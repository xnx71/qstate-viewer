// p02_make_fixtures.cpp -- generate synthetic ProposalVoting<> state blobs ("golden files") with the REAL Qubic core
// code (ProposalWithAllVoteData::set / setVoteValue / getVoteValue, QpiContextProposalFunctionCall::getVotingSummary,
// ProposalAndVotingByShareholders::getVoterId / getVoteCount) plus a canonical text dump of what the real code reads
// back. p02_decode_proposals.py --canonical must reproduce the dump byte for byte from the blob alone.
//
// Needed because the epoch-229 sample files contain no shareholder proposal (QUTIL proposals are all-zero) and no
// scalar-vote storage (ProposalDataV1<true> is only used by the test contract TESTEXB).
//
// Build: see p02_build_fixtures.sh (same flags/overlay as the oracle).
#define NO_UEFI
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "platform/memory.h"
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi.h"
#include "qpi_proposals_impl_patched.h"

// --- minimal runtime the instantiated member functions need (same semantics as test/stdlib_impl.cpp and
//     src/qpi/impl/qpi_trivial_impl.h:16-48)
void setMem(void* buffer, unsigned long long size, unsigned char value) { memset(buffer, value, size); }
void copyMem(void* destination, const void* source, unsigned long long length) { memcpy(destination, source, length); }
static void addDebugMessageAssert(const char* message, const char* file, const unsigned int lineNumber)
{
    fprintf(stderr, "ASSERT failed: %s (%s:%u)\n", message, file, lineNumber);
    abort();
}
namespace QPI
{
    template <typename T1, typename T2> inline void copyMemory(T1& dst, const T2& src)
    {
        static_assert(sizeof(dst) == sizeof(src), "Size of source and destination must match to run copyMemory().");
        copyMem(&dst, &src, sizeof(dst));
    }
    template <typename T> inline void setMemory(T& dst, uint8 value) { setMem(&dst, sizeof(dst), value); }
}

using namespace QPI;

static std::string hex(const void* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { unsigned char c = ((const unsigned char*)p)[i]; s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

// fake context: none of the functions called below touches it
static unsigned char fakeQpiBuffer[4096];
static const QpiContextFunctionCall& fakeQpi() { return *reinterpret_cast<const QpiContextFunctionCall*>(fakeQpiBuffer); }

template <typename PV> struct Fixture
{
    PV* pv;
    Fixture() { pv = (PV*)calloc(1, sizeof(PV)); }   // contract states are zero-initialized
    ~Fixture() { free(pv); }

    // what QpiContextProposalProcedureCall::setProposal does after the checks (qpi_proposals_impl.h:598-603)
    void setProposal(unsigned idx, const id& proposer, typename PV::ProposalDataType proposal, const char* url, uint16 epoch, uint32 tick)
    {
        memset(&proposal.url, 0, sizeof(proposal.url));
        memcpy(&proposal.url, url, strlen(url));
        pv->proposersAndVoters.currentProposalProposers[idx] = proposer;
        if (!pv->proposals[idx].set(proposal)) { fprintf(stderr, "set() failed for slot %u\n", idx); abort(); }
        pv->proposals[idx].tick = tick;
        pv->proposals[idx].epoch = epoch;
    }
    void vote(unsigned idx, unsigned voteIndex, sint64 value)
    {
        if (!pv->proposals[idx].setVoteValue(voteIndex, value)) { fprintf(stderr, "setVoteValue(%u,%u,%lld) failed\n", idx, voteIndex, (long long)value); abort(); }
    }
    void write(const char* path) const
    {
        FILE* f = fopen(path, "wb");
        if (!f || fwrite(pv, sizeof(PV), 1, f) != 1) { perror(path); abort(); }
        fclose(f);
    }
    // canonical dump produced with the real read functions
    void dump(FILE* out) const
    {
        QpiContextProposalFunctionCall<typename PV::ProposerAndVoterHandlingType, typename PV::ProposalDataType> api(fakeQpi(), *pv);
        fprintf(out, "sizeof %zu maxProposals %u maxVotes %u\n", sizeof(PV), (unsigned)PV::maxProposals, (unsigned)PV::maxVotes);
        for (unsigned i = 0; i < PV::maxProposals; ++i)
        {
            typename PV::ProposalDataType p;
            if (!api.getProposal(i, p))
                continue;
            fprintf(out, "slot %u\n", i);
            id proposer = pv->proposersAndVoters.currentProposalProposers[i];
            fprintf(out, "proposer %s\n", hex(&proposer, 32).c_str());
            fprintf(out, "url %s\n", (const char*)&p.url);
            fprintf(out, "epoch %u type 0x%04x tick %u\n", (unsigned)p.epoch, (unsigned)p.type, (unsigned)p.tick);
            fprintf(out, "data %s\n", hex(&p.data, sizeof(p.data)).c_str());
            fprintf(out, "votes ");
            for (unsigned v = 0; v < PV::maxVotes; ++v)
            {
                ProposalSingleVoteDataV1 sv;
                if (!api.getVote(i, v, sv)) abort();
                if (sv.voteValue == NO_VOTE_VALUE) fprintf(out, "-"); else fprintf(out, "%lld", (long long)sv.voteValue);
                fprintf(out, v + 1 < PV::maxVotes ? "," : "\n");
            }
            ProposalSummarizedVotingDataV1 s;
            memset(&s, 0, sizeof(s));
            if (!api.getVotingSummary(i, s)) abort();
            fprintf(out, "summary authorized=%u casted=%u optionCount=%u ", s.totalVotesAuthorized, s.totalVotesCasted, (unsigned)s.optionCount);
            if (s.optionCount == 0)
                fprintf(out, "scalar=%lld\n", (long long)s.scalarVotingResult);
            else
            {
                fprintf(out, "hist=");
                for (unsigned k = 0; k < 8; ++k) fprintf(out, k < 7 ? "%u," : "%u\n", s.optionVoteCount.get(k));
            }
            fprintf(out, "mostVoted=%d accepted=%d\n", (int)s.getMostVotedOption(), (int)s.getAcceptedOption());
            // only for ProposalAndVotingByShareholders (computor handling needs qpi.computor() = live computor list)
            if constexpr (requires(PV* q) { q->proposersAndVoters.currentProposalShareholders; })
            {
                fprintf(out, "voters ");
                unsigned v = 0;
                while (v < PV::maxVotes)
                {
                    id voter = pv->proposersAndVoters.getVoterId(fakeQpi(), v, i);
                    unsigned first = pv->proposersAndVoters.getVoteIndex(fakeQpi(), voter, i);
                    unsigned cnt = pv->proposersAndVoters.getVoteCount(fakeQpi(), v, i);
                    fprintf(out, "%sx%u@%u;", hex(&voter, 32).c_str(), cnt, first);
                    v += cnt;
                }
                fprintf(out, "\n");
            }
        }
    }
};

struct Holder { id who; unsigned shares; };

// what ProposalAndVotingByShareholders::setupNewProposal stores: one entry per share, holders sorted ascending (operator<)
template <typename PV> static void setShareholders(PV* pv, unsigned idx, const Holder* holders, unsigned n)
{
    unsigned v = 0;
    for (unsigned h = 0; h < n; ++h)
    {
        if (h > 0 && !(holders[h - 1].who < holders[h].who)) { fprintf(stderr, "holders not sorted\n"); abort(); }
        for (unsigned s = 0; s < holders[h].shares; ++s)
            pv->proposersAndVoters.currentProposalShareholders[idx][v++] = holders[h].who;
    }
    if (v != NUMBER_OF_COMPUTORS) { fprintf(stderr, "shares must sum to 676, got %u\n", v); abort(); }
}

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <outdir>\n", argv[0]); return 1; }
    std::string dir = argv[1];

    // ------------------------------------------------------------------------------------------------------------
    // Fixture A: QUTIL-like  ProposalVoting<ProposalAndVotingByShareholders<8, asset>, ProposalDataYesNo>
    {
        typedef ProposalVoting<ProposalAndVotingByShareholders<8, 327647778129ULL>, ProposalDataYesNo> PV;
        Fixture<PV> fx;
        const Holder holders1[] = {
            { id(1, 2, 3, 4), 300 }, { id(5, 0, 0, 1), 200 }, { id(5, 0, 0, 2), 100 },
            { id(0x1111111111111111ULL, 9, 9, 9), 75 }, { id(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0, 1), 1 } };
        ProposalDataYesNo p;
        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::VariableYesNo;
        p.data.variableOptions.variable = 3;
        p.data.variableOptions.value = 77;
        fx.setProposal(1, holders1[2].who, p, "https://example.org/fixtureA/slot1-variable-yes-no", 229, 77700123);
        setShareholders(fx.pv, 1, holders1, 5);
        for (unsigned v = 0; v < 300; ++v) fx.vote(1, v, 1);          // holder 0: all yes
        for (unsigned v = 300; v < 450; ++v) fx.vote(1, v, 1);        // holder 1: 150 yes
        for (unsigned v = 450; v < 500; ++v) fx.vote(1, v, 0);        //           50 no
        /* holder 2 (500..599): no vote */
        for (unsigned v = 600; v < 675; ++v) fx.vote(1, v, 0);        // holder 3: all no
        fx.vote(1, 675, 1);                                           // holder 4: yes

        const Holder holders5[] = { { id(42, 42, 42, 42), 676 } };
        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::MultiVariablesThreeOptions;           // 3 options are allowed with 2-bit storage
        fx.setProposal(5, holders5[0].who, p, "https://example.org/fixtureA/slot5-multivar-3-options", 228, 77000001);
        setShareholders(fx.pv, 5, holders5, 1);
        for (unsigned v = 0; v < 676; ++v) if (v % 5 != 4) fx.vote(5, v, v % 3);
        fx.vote(5, 10, NO_VOTE_VALUE);                                // withdraw a vote again

        const Holder holders7[] = { { id(7, 0, 0, 0), 338 }, { id(8, 0, 0, 0), 338 } };
        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::TransferYesNo;
        p.data.transfer.destination = id(0x0123456789abcdefULL, 0xfedcba9876543210ULL, 0x1122334455667788ULL, 0x99aabbccddeeff00ULL);
        p.data.transfer.amount = 1234567890123LL;
        fx.setProposal(7, holders7[1].who, p, "https://example.org/fixtureA/slot7-transfer-yes-no", 229, 77700999);
        setShareholders(fx.pv, 7, holders7, 2);

        fx.write((dir + "/fixtureA_shareholders8_yesno.bin").c_str());
        FILE* f = fopen((dir + "/fixtureA_shareholders8_yesno.expected.txt").c_str(), "w");
        fx.dump(f);
        fclose(f);
        printf("fixtureA: %zu bytes\n", sizeof(PV));
    }

    // ------------------------------------------------------------------------------------------------------------
    // Fixture B: TESTEXB-like  ProposalVoting<ProposalAndVotingByShareholders<16, asset>, ProposalDataV1<true>>
    {
        typedef ProposalVoting<ProposalAndVotingByShareholders<16, 18674403253634388ULL>, ProposalDataV1<true>> PV;
        Fixture<PV> fx;
        const Holder holders[] = { { id(10, 0, 0, 0), 100 }, { id(20, 0, 0, 0), 250 }, { id(30, 0, 0, 0), 26 }, { id(40, 0, 0, 0), 300 } };
        ProposalDataV1<true> p;

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::VariableScalarMean;
        p.data.variableScalar.variable = 1;
        p.data.variableScalar.minValue = -1000;
        p.data.variableScalar.maxValue = 1000000;
        p.data.variableScalar.proposedValue = 500;
        fx.setProposal(0, holders[0].who, p, "https://example.org/fixtureB/slot0-scalar-regular-mean", 229, 77700001);
        setShareholders(fx.pv, 0, holders, 4);
        for (unsigned v = 0; v < 100; ++v) fx.vote(0, v, -1000 + (sint64)v);
        for (unsigned v = 100; v < 350; ++v) fx.vote(0, v, (v % 2) ? 999999 : -7);
        for (unsigned v = 376; v < 500; ++v) fx.vote(0, v, 0);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::VariableScalarMean;
        p.data.variableScalar.variable = 2;
        p.data.variableScalar.minValue = p.data.variableScalar.minSupportedValue;
        p.data.variableScalar.maxValue = p.data.variableScalar.maxSupportedValue;
        p.data.variableScalar.proposedValue = 0;
        fx.setProposal(2, holders[1].who, p, "https://example.org/fixtureB/slot2-scalar-overflow-safe-mean", 229, 77700002);
        setShareholders(fx.pv, 2, holders, 4);
        for (unsigned v = 0; v < 40; ++v) fx.vote(2, v, 0x7fffffffffffffffLL - (sint64)v * 1000003);
        for (unsigned v = 40; v < 71; ++v) fx.vote(2, v, (sint64)0x8000000000000001LL + (sint64)v * 999983);
        for (unsigned v = 200; v < 223; ++v) fx.vote(2, v, -5 - (sint64)v);
        fx.vote(2, 675, 1);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::VariableFourValues;
        p.data.variableOptions.variable = 0;
        p.data.variableOptions.values.set(0, 10); p.data.variableOptions.values.set(1, 20);
        p.data.variableOptions.values.set(2, 30); p.data.variableOptions.values.set(3, 40);
        fx.setProposal(3, holders[2].who, p, "https://example.org/fixtureB/slot3-variable-four-values", 228, 77000003);
        setShareholders(fx.pv, 3, holders, 4);
        for (unsigned v = 0; v < 676; ++v) if (v % 7 != 6) fx.vote(3, v, v % 5);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::type(ProposalTypes::Class::GeneralOptions, 8);
        fx.setProposal(4, holders[3].who, p, "https://example.org/fixtureB/slot4-general-8-options", 229, 77700004);
        setShareholders(fx.pv, 4, holders, 4);
        for (unsigned v = 0; v < 676; ++v) if (v % 9 != 8) fx.vote(4, v, v % 9);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::TransferInEpochYesNo;
        p.data.transferInEpoch.destination = id(9, 0, 0, 0);
        p.data.transferInEpoch.amount = 150000;
        p.data.transferInEpoch.targetEpoch = 240;
        fx.setProposal(9, holders[0].who, p, "https://example.org/fixtureB/slot9-transfer-in-epoch", 229, 77700009);
        setShareholders(fx.pv, 9, holders, 4);
        for (unsigned v = 0; v < 500; ++v) fx.vote(9, v, 1);
        for (unsigned v = 500; v < 520; ++v) fx.vote(9, v, 0);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::TransferThreeAmounts;
        p.data.transfer.destination = id(1, 1, 1, 1);
        p.data.transfer.amounts.set(0, 100); p.data.transfer.amounts.set(1, 200); p.data.transfer.amounts.set(2, 300);
        fx.setProposal(15, holders[1].who, p, "https://example.org/fixtureB/slot15-transfer-three-amounts", 229, 77700015);
        setShareholders(fx.pv, 15, holders, 4);
        for (unsigned v = 0; v < 676; ++v) fx.vote(15, v, (v * 7) % 4);

        fx.write((dir + "/fixtureB_shareholders16_v1scalar.bin").c_str());
        FILE* f = fopen((dir + "/fixtureB_shareholders16_v1scalar.expected.txt").c_str(), "w");
        fx.dump(f);
        fclose(f);
        printf("fixtureB: %zu bytes\n", sizeof(PV));
    }

    // ------------------------------------------------------------------------------------------------------------
    // Fixture C: ProposalVoting<ProposalByAnyoneVotingByComputors<200>, ProposalDataV1<false>> (inherited handling type)
    {
        typedef ProposalVoting<ProposalByAnyoneVotingByComputors<200>, ProposalDataV1<false>> PV;
        Fixture<PV> fx;
        ProposalDataV1<false> p;

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::YesNo;
        fx.setProposal(0, id(100, 200, 300, 400), p, "https://example.org/fixtureC/slot0-yes-no", 229, 77700100);
        for (unsigned v = 0; v < 676; ++v) if (v % 3) fx.vote(0, v, v % 2);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::TransferFourAmounts;
        p.data.transfer.destination = id(6, 0, 0, 0);
        p.data.transfer.amounts.set(0, 1000); p.data.transfer.amounts.set(1, 20000); p.data.transfer.amounts.set(2, 300000); p.data.transfer.amounts.set(3, 1000000);
        fx.setProposal(100, id(1, 0, 0, 0), p, "https://example.org/fixtureC/slot100-transfer-four-amounts", 227, 75000100);
        for (unsigned v = 0; v < 600; ++v) fx.vote(100, v, v % 5);

        memset(&p, 0, sizeof(p));
        p.type = ProposalTypes::type(ProposalTypes::Class::MultiVariables, 8);
        fx.setProposal(199, id(0xdeadbeefULL, 0, 0, 0xcafeULL), p, "https://example.org/fixtureC/slot199-multivar-8-options", 229, 77700199);
        for (unsigned v = 0; v < 676; ++v) if (v % 9 != 8) fx.vote(199, v, v % 9);

        fx.write((dir + "/fixtureC_anyone200_v1.bin").c_str());
        FILE* f = fopen((dir + "/fixtureC_anyone200_v1.expected.txt").c_str(), "w");
        fx.dump(f);
        fclose(f);
        printf("fixtureC: %zu bytes\n", sizeof(PV));
    }
    return 0;
}
