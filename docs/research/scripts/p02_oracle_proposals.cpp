// p02_oracle_proposals.cpp -- ground-truth sizes/offsets of the QPI proposal types, obtained by compiling the REAL
// Qubic core headers (qpi.h, qpi_proposals.h, qpi_proposals_impl.h) and the REAL contract files with g++.
//
// Build (see p02_build_oracle.sh):
//   g++ -std=c++20 -mavx2 -mbmi2 -fno-access-control -Wno-invalid-offsetof -w \
//       -I <overlay> -I <core>/src -I <core> p02_oracle_proposals.cpp -o oracle
// <overlay>/qpi_proposals_impl_patched.h is a copy of src/qpi/impl/qpi_proposals_impl.h where the single expression
// "pv.maxVotes" in a template argument (line 971, inside a member function body, irrelevant for layout) is replaced by
// "ProposalVotingType::maxVotes", because g++ 13 rejects "use of 'this' in a constant expression" (P2280 not implemented).
#define NO_UEFI
#include <cstdio>                 // TestExampleB.h uses printf under NO_UEFI
#include "platform/memory.h"      // setMem() used by ProposalAndVotingByShareholders::freeProposalByIndex (found via ADL)
#include "contract_core/pre_qpi_def.h"
#include "qpi/qpi.h"
#include "qpi_proposals_impl_patched.h"
#include "oracle_core/oracle_interfaces_def.h"
#include "oc_core/oc_interfaces_def.h"

#define QX_CONTRACT_INDEX 1
#define CONTRACT_INDEX QX_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QX
#define CONTRACT_STATE2_TYPE QX2
#include "contracts/Qx.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define QUTIL_CONTRACT_INDEX 4
#define CONTRACT_INDEX QUTIL_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE QUTIL
#define CONTRACT_STATE2_TYPE QUTIL2
#include "contracts/QUtil.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define GQMPROP_CONTRACT_INDEX 6
#define CONTRACT_INDEX GQMPROP_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE GQMPROP
#define CONTRACT_STATE2_TYPE GQMPROP2
#include "contracts/GeneralQuorumProposal.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#define CCF_CONTRACT_INDEX 8
#define CONTRACT_INDEX CCF_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE CCF
#define CONTRACT_STATE2_TYPE CCF2
#include "contracts/ComputorControlledFund.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

constexpr unsigned short TESTEXA_CONTRACT_INDEX = 100;
#define CONTRACT_INDEX TESTEXA_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE TESTEXA
#define CONTRACT_STATE2_TYPE TESTEXA2
#include "contracts/TestExampleA.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

constexpr unsigned short TESTEXB_CONTRACT_INDEX = 101;
#define CONTRACT_INDEX TESTEXB_CONTRACT_INDEX
#define CONTRACT_STATE_TYPE TESTEXB
#define CONTRACT_STATE2_TYPE TESTEXB2
#include "contracts/TestExampleB.h"
#undef CONTRACT_INDEX
#undef CONTRACT_STATE_TYPE
#undef CONTRACT_STATE2_TYPE

#include <cstdio>
#include <cstddef>
#include <type_traits>

#define SZ(T) printf("sizeof %-78s = %8zu  align %2zu\n", #T, sizeof(T), alignof(T))
#define OFF(T, m) printf("  offsetof %-70s = %8zu  size %8zu\n", #T "::" #m, offsetof(T, m), sizeof(((T*)0)->m))
#define VAL(e) printf("value  %-78s = %lld\n", #e, (long long)(e))

using namespace QPI;

template <typename PV> static void dumpPV(const char* name)
{
    printf("== %s\n", name);
    printf("  sizeof=%zu align=%zu maxProposals=%u maxVotes=%u\n", sizeof(PV), alignof(PV), (unsigned)PV::maxProposals, (unsigned)PV::maxVotes);
    printf("  offsetof proposersAndVoters=%zu size=%zu\n", offsetof(PV, proposersAndVoters), sizeof(typename PV::ProposerAndVoterHandlingType));
    printf("  offsetof proposals=%zu elemSize=%zu count=%zu\n", offsetof(PV, proposals), sizeof(typename PV::ProposalAndVotesDataType), sizeof(((PV*)0)->proposals) / sizeof(typename PV::ProposalAndVotesDataType));
    typedef typename PV::ProposalAndVotesDataType E;
    printf("  elem: offsetof votes=%zu sizeof(votes)=%zu sizeof(votes[0])=%zu baseSize=%zu\n", offsetof(E, votes), sizeof(((E*)0)->votes), sizeof(((E*)0)->votes[0]), sizeof(typename PV::ProposalDataType));
}

int main()
{
    VAL(NUMBER_OF_COMPUTORS); VAL(QUORUM); VAL(INVALID_PROPOSAL_INDEX); VAL(INVALID_VOTE_INDEX); VAL(NO_VOTE_VALUE);
    SZ(id); SZ(bit); SZ(NoData);

    SZ(ProposalSingleVoteDataV1);
    OFF(ProposalSingleVoteDataV1, proposalIndex); OFF(ProposalSingleVoteDataV1, proposalType); OFF(ProposalSingleVoteDataV1, proposalTick); OFF(ProposalSingleVoteDataV1, voteValue);
    SZ(ProposalMultiVoteDataV1);
    OFF(ProposalMultiVoteDataV1, proposalIndex); OFF(ProposalMultiVoteDataV1, proposalType); OFF(ProposalMultiVoteDataV1, proposalTick); OFF(ProposalMultiVoteDataV1, voteValues); OFF(ProposalMultiVoteDataV1, voteCounts);
    SZ(ProposalSummarizedVotingDataV1);
    OFF(ProposalSummarizedVotingDataV1, proposalIndex); OFF(ProposalSummarizedVotingDataV1, optionCount); OFF(ProposalSummarizedVotingDataV1, proposalTick);
    OFF(ProposalSummarizedVotingDataV1, totalVotesAuthorized); OFF(ProposalSummarizedVotingDataV1, totalVotesCasted); OFF(ProposalSummarizedVotingDataV1, optionVoteCount); OFF(ProposalSummarizedVotingDataV1, scalarVotingResult);

    SZ(ProposalDataV1<true>); SZ(ProposalDataV1<false>);
    OFF(ProposalDataV1<false>, url); OFF(ProposalDataV1<false>, epoch); OFF(ProposalDataV1<false>, type); OFF(ProposalDataV1<false>, tick); OFF(ProposalDataV1<false>, data);
    SZ(ProposalDataV1<false>::Data); SZ(ProposalDataV1<true>::Data);
    SZ(ProposalDataV1<false>::Data::Transfer);
    OFF(ProposalDataV1<false>::Data::Transfer, destination); OFF(ProposalDataV1<false>::Data::Transfer, amounts);
    SZ(ProposalDataV1<false>::Data::TransferInEpoch);
    OFF(ProposalDataV1<false>::Data::TransferInEpoch, destination); OFF(ProposalDataV1<false>::Data::TransferInEpoch, amount); OFF(ProposalDataV1<false>::Data::TransferInEpoch, targetEpoch);
    SZ(ProposalDataV1<false>::Data::VariableOptions);
    OFF(ProposalDataV1<false>::Data::VariableOptions, variable); OFF(ProposalDataV1<false>::Data::VariableOptions, values);
    SZ(ProposalDataV1<false>::Data::VariableScalar);
    OFF(ProposalDataV1<false>::Data::VariableScalar, variable); OFF(ProposalDataV1<false>::Data::VariableScalar, minValue); OFF(ProposalDataV1<false>::Data::VariableScalar, maxValue); OFF(ProposalDataV1<false>::Data::VariableScalar, proposedValue);
    OFF(ProposalDataV1<false>::Data, transfer); OFF(ProposalDataV1<false>::Data, transferInEpoch); OFF(ProposalDataV1<false>::Data, variableOptions); OFF(ProposalDataV1<false>::Data, variableScalar);

    SZ(ProposalDataYesNo);
    OFF(ProposalDataYesNo, url); OFF(ProposalDataYesNo, epoch); OFF(ProposalDataYesNo, type); OFF(ProposalDataYesNo, tick); OFF(ProposalDataYesNo, data);
    SZ(ProposalDataYesNo::Data);
    SZ(ProposalDataYesNo::Data::Transfer);
    OFF(ProposalDataYesNo::Data::Transfer, destination); OFF(ProposalDataYesNo::Data::Transfer, amount);
    SZ(ProposalDataYesNo::Data::VariableOptions);
    OFF(ProposalDataYesNo::Data::VariableOptions, variable); OFF(ProposalDataYesNo::Data::VariableOptions, value);

    SZ(__VoteStorageTypeSelector<false>::type); SZ(__VoteStorageTypeSelector<true>::type);
    SZ(__VoteStorageTypeSelector<false>); SZ(__VoteStorageTypeSelector<true>);

    typedef ProposalWithAllVoteData<ProposalDataV1<false>, 676> PWAV_V1F_676;
    typedef ProposalWithAllVoteData<ProposalDataV1<true>, 676> PWAV_V1T_676;
    typedef ProposalWithAllVoteData<ProposalDataYesNo, 676> PWAV_YN_676;
    typedef ProposalWithAllVoteData<ProposalDataV1<false>, 42> PWAV_V1F_42;
    typedef ProposalWithAllVoteData<ProposalDataV1<true>, 42> PWAV_V1T_42;
    typedef ProposalWithAllVoteData<ProposalDataYesNo, 42> PWAV_YN_42;
    typedef ProposalWithAllVoteData<ProposalDataYesNo, 1> PWAV_YN_1;
    typedef ProposalWithAllVoteData<ProposalDataYesNo, 4> PWAV_YN_4;
    typedef ProposalWithAllVoteData<ProposalDataYesNo, 5> PWAV_YN_5;
    SZ(PWAV_V1F_676); OFF(PWAV_V1F_676, votes); OFF(PWAV_V1F_676, url); OFF(PWAV_V1F_676, epoch); OFF(PWAV_V1F_676, type); OFF(PWAV_V1F_676, tick); OFF(PWAV_V1F_676, data);
    SZ(PWAV_V1T_676); OFF(PWAV_V1T_676, votes);
    SZ(PWAV_YN_676); OFF(PWAV_YN_676, votes); OFF(PWAV_YN_676, url); OFF(PWAV_YN_676, epoch); OFF(PWAV_YN_676, type); OFF(PWAV_YN_676, tick); OFF(PWAV_YN_676, data);
    SZ(PWAV_V1F_42); OFF(PWAV_V1F_42, votes);
    SZ(PWAV_V1T_42); OFF(PWAV_V1T_42, votes);
    SZ(PWAV_YN_42); OFF(PWAV_YN_42, votes);
    SZ(PWAV_YN_1); OFF(PWAV_YN_1, votes);
    SZ(PWAV_YN_4); OFF(PWAV_YN_4, votes);
    SZ(PWAV_YN_5); OFF(PWAV_YN_5, votes);
    VAL(PWAV_V1F_676::supportScalarVotes); VAL(PWAV_V1T_676::supportScalarVotes);
    VAL(std::is_standard_layout<PWAV_V1F_676>::value); VAL(std::is_standard_layout<PWAV_YN_676>::value);
    VAL(std::is_trivially_copyable<ProposalDataYesNo>::value); VAL(std::is_trivially_copyable<ProposalDataV1<false>>::value);
    VAL(std::is_standard_layout<ProposalDataYesNo>::value); VAL(std::is_standard_layout<ProposalDataV1<false>>::value);

    SZ(ProposalAndVotingByComputors<>); VAL(ProposalAndVotingByComputors<>::maxProposals); VAL(ProposalAndVotingByComputors<>::maxVotes);
    SZ(ProposalAndVotingByComputors<676>); OFF(ProposalAndVotingByComputors<676>, currentProposalProposers);
    SZ(ProposalAndVotingByComputors<100>); OFF(ProposalAndVotingByComputors<100>, currentProposalProposers);
    SZ(ProposalAndVotingByComputors<200>);
    SZ(ProposalByAnyoneVotingByComputors<200>); OFF(ProposalByAnyoneVotingByComputors<200>, currentProposalProposers);
    VAL(ProposalByAnyoneVotingByComputors<200>::maxProposals); VAL(ProposalByAnyoneVotingByComputors<200>::maxVotes);
    typedef ProposalAndVotingByShareholders<8, QUTIL_CONTRACT_ASSET_NAME> PAVS_QUTIL;
    typedef ProposalAndVotingByShareholders<16, TESTEXA_ASSET_NAME> PAVS_16;
    typedef ProposalAndVotingByShareholders<3, 1> PAVS_3;
    SZ(PAVS_QUTIL); OFF(PAVS_QUTIL, currentProposalProposers); OFF(PAVS_QUTIL, currentProposalShareholders);
    SZ(PAVS_16); OFF(PAVS_16, currentProposalProposers); OFF(PAVS_16, currentProposalShareholders);
    SZ(PAVS_3); OFF(PAVS_3, currentProposalProposers); OFF(PAVS_3, currentProposalShareholders);
    VAL(PAVS_QUTIL::maxProposals); VAL(PAVS_QUTIL::maxVotes);

    dumpPV<GQMPROP::ProposalVotingT>("GQMPROP::ProposalVotingT = ProposalVoting<ProposalAndVotingByComputors<NUMBER_OF_COMPUTORS>, ProposalDataV1<false>>");
    dumpPV<CCF::ProposalVotingT>("CCF::ProposalVotingT = ProposalVoting<ProposalAndVotingByComputors<100>, ProposalDataYesNo>");
    dumpPV<QUTIL::ProposalVotingT>("QUTIL::ProposalVotingT = ProposalVoting<ProposalAndVotingByShareholders<8, QUTIL_CONTRACT_ASSET_NAME>, ProposalDataYesNo>");
    dumpPV<TESTEXA::ProposalVotingT>("TESTEXA::ProposalVotingT = ProposalVoting<ProposalAndVotingByShareholders<16, TESTEXA_ASSET_NAME>, ProposalDataYesNo>");
    dumpPV<TESTEXB::ProposalVotingT>("TESTEXB::ProposalVotingT = ProposalVoting<ProposalAndVotingByShareholders<16, TESTEXB_ASSET_NAME>, ProposalDataV1<true>>");
    dumpPV<ProposalVoting<ProposalAndVotingByComputors<200>, ProposalDataV1<true>>>("test: ProposalVoting<ProposalAndVotingByComputors<200>, ProposalDataV1<true>>");
    dumpPV<ProposalVoting<ProposalByAnyoneVotingByComputors<200>, ProposalDataV1<false>>>("test: ProposalVoting<ProposalByAnyoneVotingByComputors<200>, ProposalDataV1<false>>");

    VAL(QUTIL_CONTRACT_ASSET_NAME); VAL(TESTEXA_ASSET_NAME); VAL(TESTEXB_ASSET_NAME);

    SZ(GQMPROP::StateData); OFF(GQMPROP::StateData, proposals); OFF(GQMPROP::StateData, revenueDonation);
    SZ(GQMPROP::RevenueDonationEntry); OFF(GQMPROP::RevenueDonationEntry, destinationPublicKey); OFF(GQMPROP::RevenueDonationEntry, millionthAmount); OFF(GQMPROP::RevenueDonationEntry, firstEpoch);

    SZ(CCF::StateData); OFF(CCF::StateData, proposals); OFF(CCF::StateData, latestTransfers); OFF(CCF::StateData, lastTransfersNextOverwriteIdx); OFF(CCF::StateData, setProposalFee);
    OFF(CCF::StateData, regularPayments); OFF(CCF::StateData, subscriptionProposals); OFF(CCF::StateData, activeSubscriptions); OFF(CCF::StateData, lastRegularPaymentsNextOverwriteIdx);
    SZ(CCF::LatestTransfersEntry); OFF(CCF::LatestTransfersEntry, destination); OFF(CCF::LatestTransfersEntry, url); OFF(CCF::LatestTransfersEntry, amount); OFF(CCF::LatestTransfersEntry, tick); OFF(CCF::LatestTransfersEntry, success);
    SZ(CCF::RegularPaymentEntry); OFF(CCF::RegularPaymentEntry, destination); OFF(CCF::RegularPaymentEntry, url); OFF(CCF::RegularPaymentEntry, amount); OFF(CCF::RegularPaymentEntry, tick); OFF(CCF::RegularPaymentEntry, periodIndex); OFF(CCF::RegularPaymentEntry, success);
    SZ(CCF::SubscriptionProposalData); OFF(CCF::SubscriptionProposalData, proposerId); OFF(CCF::SubscriptionProposalData, destination); OFF(CCF::SubscriptionProposalData, url); OFF(CCF::SubscriptionProposalData, weeksPerPeriod);
    OFF(CCF::SubscriptionProposalData, numberOfPeriods); OFF(CCF::SubscriptionProposalData, amountPerPeriod); OFF(CCF::SubscriptionProposalData, startEpoch);
    SZ(CCF::SubscriptionData); OFF(CCF::SubscriptionData, destination); OFF(CCF::SubscriptionData, url); OFF(CCF::SubscriptionData, weeksPerPeriod); OFF(CCF::SubscriptionData, numberOfPeriods);
    OFF(CCF::SubscriptionData, amountPerPeriod); OFF(CCF::SubscriptionData, startEpoch); OFF(CCF::SubscriptionData, currentPeriod);

    SZ(QUTIL::StateData); OFF(QUTIL::StateData, polls); OFF(QUTIL::StateData, voters); OFF(QUTIL::StateData, poll_ids); OFF(QUTIL::StateData, voter_counts); OFF(QUTIL::StateData, poll_links);
    OFF(QUTIL::StateData, current_poll_id); OFF(QUTIL::StateData, dfMiningSeed); OFF(QUTIL::StateData, smt1InvocationFee); OFF(QUTIL::StateData, shareholderProposalFee); OFF(QUTIL::StateData, _futureFeePlaceholder0); OFF(QUTIL::StateData, proposals);

    SZ(TESTEXA::StateData); OFF(TESTEXA::StateData, proposals); OFF(TESTEXA::StateData, multiVariablesProposalData); OFF(TESTEXA::StateData, dummyStateVariable3);
    SZ(TESTEXB::StateData); OFF(TESTEXB::StateData, proposals); OFF(TESTEXB::StateData, fee3);
    return 0;
}
