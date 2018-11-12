/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-present Facebook, Inc. (http://www.facebook.com)  |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/unit-emitter.h"

#include "hphp/compiler/analysis/analysis_result.h"
#include "hphp/compiler/builtin_symbols.h"
#include "hphp/compiler/option.h"
#include "hphp/parser/location.h"
#include "hphp/system/systemlib.h"

#include "hphp/runtime/base/array-data.h"
#include "hphp/runtime/base/attr.h"
#include "hphp/runtime/base/file-util.h"
#include "hphp/runtime/base/runtime-option.h"
#include "hphp/runtime/base/static-string-table.h"
#include "hphp/runtime/base/typed-value.h"
#include "hphp/runtime/base/repo-auth-type-array.h"
#include "hphp/runtime/base/variable-serializer.h"

#include "hphp/runtime/ext/std/ext_std_variable.h"

#include "hphp/runtime/vm/blob-helper.h"
#include "hphp/runtime/vm/disas.h"
#include "hphp/runtime/vm/func.h"
#include "hphp/runtime/vm/func-emitter.h"
#include "hphp/runtime/vm/jit/perf-counters.h"
#include "hphp/runtime/vm/litstr-table.h"
#include "hphp/runtime/vm/native.h"
#include "hphp/runtime/vm/preclass.h"
#include "hphp/runtime/vm/preclass-emitter.h"
#include "hphp/runtime/vm/repo.h"
#include "hphp/runtime/vm/repo-helpers.h"
#include "hphp/runtime/vm/unit.h"
#include "hphp/runtime/vm/verifier/check.h"

#include "hphp/util/logger.h"
#include "hphp/util/md5.h"
#include "hphp/util/read-only-arena.h"
#include "hphp/util/trace.h"

#include <boost/algorithm/string/predicate.hpp>

#include <folly/Memory.h>
#include <folly/FileUtil.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

TRACE_SET_MOD(hhbc);

using MergeKind = Unit::MergeKind;

///////////////////////////////////////////////////////////////////////////////

static ReadOnlyArena& get_readonly_arena() {
  static ReadOnlyArena arena(RuntimeOption::EvalHHBCArenaChunkSize);
  return arena;
}

/*
 * Export for the admin server.
 */
size_t hhbc_arena_capacity() {
  if (!RuntimeOption::RepoAuthoritative) return 0;
  return get_readonly_arena().capacity();
}

///////////////////////////////////////////////////////////////////////////////

UnitEmitter::UnitEmitter(const MD5& md5, const Native::FuncTable& nativeFuncs)
  : m_mainReturn(make_tv<KindOfUninit>())
  , m_nativeFuncs(nativeFuncs)
  , m_md5(md5)
  , m_bc((unsigned char*)malloc(BCMaxInit))
  , m_bclen(0)
  , m_bcmax(BCMaxInit)
  , m_nextFuncSn(0)
  , m_allClassesHoistable(true)
{}

UnitEmitter::~UnitEmitter() {
  if (m_bc) free(m_bc);

  for (auto& fe : m_fes) delete fe;
  for (auto& pce : m_pceVec) delete pce;
}


///////////////////////////////////////////////////////////////////////////////
// Basic data.

void UnitEmitter::setBc(const unsigned char* bc, size_t bclen) {
  if (m_bc) {
    free(m_bc);
  }
  m_bc = (unsigned char*)malloc(bclen);
  m_bcmax = bclen;
  memcpy(m_bc, bc, bclen);
  m_bclen = bclen;
}


///////////////////////////////////////////////////////////////////////////////
// Litstrs and Arrays.

const StringData* UnitEmitter::lookupLitstr(Id id) const {
  if (isGlobalLitstrId(id)) {
    return LitstrTable::get().lookupLitstrId(decodeGlobalLitstrId(id));
  }
  assertx(id < m_litstrs.size());
  return m_litstrs[id];
}

const ArrayData* UnitEmitter::lookupArray(Id id) const {
  assertx(id < m_arrays.size());
  return m_arrays[id];
}

const RepoAuthType::Array* UnitEmitter::lookupArrayType(Id id) const {
  return RuntimeOption::RepoAuthoritative
           ? globalArrayTypeTable().lookup(id)
           : m_arrayTypeTable.lookup(id);
}

void UnitEmitter::repopulateArrayTypeTable(
  const ArrayTypeTable::Builder& builder) {
  m_arrayTypeTable.repopulate(builder);
}

Id UnitEmitter::mergeLitstr(const StringData* litstr) {
  if (Option::WholeProgram) {
    return encodeGlobalLitstrId(LitstrTable::get().mergeLitstr(litstr));
  }
  return mergeUnitLitstr(litstr);
}

Id UnitEmitter::mergeUnitLitstr(const StringData* litstr) {
  auto it = m_litstr2id.find(litstr);
  if (it == m_litstr2id.end()) {
    const StringData* str = makeStaticString(litstr);
    Id id = m_litstrs.size();
    m_litstrs.push_back(str);
    m_litstr2id[str] = id;
    return id;
  } else {
    return it->second;
  }
}

Id UnitEmitter::mergeArray(const ArrayData* a) {
  assertx(a->isStatic());
  auto const emplaced = m_array2id.emplace(a, static_cast<Id>(m_arrays.size()));
  if (emplaced.second) {
    m_arrays.push_back(a);
  }
  return emplaced.first->second;
}


///////////////////////////////////////////////////////////////////////////////
// FuncEmitters.

void UnitEmitter::initMain(int line1, int line2) {
  assertx(m_fes.size() == 0);
  StringData* name = staticEmptyString();
  FuncEmitter* pseudomain = newFuncEmitter(name);
  Attr attrs = AttrMayUseVV;
  pseudomain->init(line1, line2, 0, attrs, false, name);
}

void UnitEmitter::addTrivialPseudoMain() {
  initMain(0, 0);
  auto const mfe = getMain();
  emitOp(OpInt);
  emitInt64(1);
  emitOp(OpRetC);
  mfe->maxStackCells = 1;
  mfe->finish(bcPos(), false);

  TypedValue mainReturn;
  mainReturn.m_data.num = 1;
  mainReturn.m_type = KindOfInt64;
  m_mainReturn = mainReturn;
  m_mergeOnly = true;
}

FuncEmitter* UnitEmitter::newFuncEmitter(const StringData* name) {
  // The pseudomain comes first.
  assertx(m_fes.size() > 0 || !strcmp(name->data(), ""));

  FuncEmitter* fe = new FuncEmitter(*this, m_nextFuncSn++, m_fes.size(), name);
  m_fes.push_back(fe);
  return fe;
}

FuncEmitter* UnitEmitter::newMethodEmitter(const StringData* name,
                                           PreClassEmitter* pce) {
  return new FuncEmitter(*this, m_nextFuncSn++, name, pce);
}

void UnitEmitter::appendTopEmitter(FuncEmitter* fe) {
  fe->setIds(m_nextFuncSn++, m_fes.size());
  m_fes.push_back(fe);
}

Func* UnitEmitter::newFunc(const FuncEmitter* fe, Unit& unit,
                           const StringData* name, Attr attrs,
                           int numParams) {
  auto const func = new (Func::allocFuncMem(numParams)) Func(unit, name, attrs);
  if (unit.m_extended) unit.getExtended()->m_funcTable.push_back(func);
  return func;
}


///////////////////////////////////////////////////////////////////////////////
// PreClassEmitters.

void UnitEmitter::addPreClassEmitter(PreClassEmitter* pce) {
  if (pce->hoistability() && m_hoistablePreClassSet.count(pce->name())) {
    pce->setHoistable(PreClass::Mergeable);
  }
  auto hoistable = pce->hoistability();

  if (hoistable >= PreClass::MaybeHoistable) {
    m_hoistablePreClassSet.insert(pce->name());
    m_hoistablePceIdList.push_back(pce->id());
  } else {
    m_allClassesHoistable = false;
  }
  if (hoistable >= PreClass::Mergeable &&
      hoistable < PreClass::AlwaysHoistable) {
    if (m_returnSeen) {
      m_allClassesHoistable = false;
    } else {
      pushMergeableClass(pce);
    }
  }
}

PreClassEmitter* UnitEmitter::newBarePreClassEmitter(
  const std::string& name,
  PreClass::Hoistable hoistable
) {
  auto pce = new PreClassEmitter(*this, m_pceVec.size(), name, hoistable);
  m_pceVec.push_back(pce);
  return pce;
}

PreClassEmitter* UnitEmitter::newPreClassEmitter(
  const std::string& name,
  PreClass::Hoistable hoistable
) {
  PreClassEmitter* pce = newBarePreClassEmitter(name, hoistable);
  addPreClassEmitter(pce);
  return pce;
}

Id UnitEmitter::pceId(folly::StringPiece clsName) {
  Id id = 0;
  for (auto p : m_pceVec) {
    if (p->name()->slice() == clsName) return id;
    id++;
  }
  return -1;
}

///////////////////////////////////////////////////////////////////////////////
// Type aliases.

Id UnitEmitter::addTypeAlias(const TypeAlias& td) {
  Id id = m_typeAliases.size();
  m_typeAliases.push_back(td);
  return id;
}


///////////////////////////////////////////////////////////////////////////////
// Source locations.

SourceLocTable UnitEmitter::createSourceLocTable() const {
  assertx(m_sourceLocTab.size() != 0);
  SourceLocTable locations;
  for (size_t i = 0; i < m_sourceLocTab.size(); ++i) {
    Offset endOff = i < m_sourceLocTab.size() - 1
      ? m_sourceLocTab[i + 1].first
      : m_bclen;
    locations.push_back(SourceLocEntry(endOff, m_sourceLocTab[i].second));
  }
  return locations;
}

namespace {

using SrcLoc = std::vector<std::pair<Offset, SourceLoc>>;

/*
 * Create a LineTable from `srcLoc'.
 */
LineTable createLineTable(const SrcLoc& srcLoc, Offset bclen) {
  LineTable lines;
  if (srcLoc.empty()) {
    return lines;
  }

  auto prev = srcLoc.begin();
  for (auto it = prev + 1; it != srcLoc.end(); ++it) {
    if (prev->second.line1 != it->second.line1) {
      lines.push_back(LineEntry(it->first, prev->second.line1));
      prev = it;
    }
  }

  lines.push_back(LineEntry(bclen, prev->second.line1));
  return lines;
}

}

void UnitEmitter::recordSourceLocation(const Location::Range& sLoc,
                                       Offset start) {
  // Some byte codes, such as for the implicit "return 0" at the end of a
  // a source file do not have valid source locations. This check makes
  // sure we don't record a (dummy) source location in this case.
  if (start > 0 && sLoc.line0 == -1) return;
  SourceLoc newLoc(sLoc);
  if (!m_sourceLocTab.empty()) {
    if (m_sourceLocTab.back().second == newLoc) {
      // Combine into the interval already at the back of the vector.
      assertx(start >= m_sourceLocTab.back().first);
      return;
    }
    assertx(m_sourceLocTab.back().first < start &&
           "source location offsets must be added to UnitEmitter in "
           "increasing order");
  } else {
    // First record added should be for bytecode offset zero or very rarely one
    // when the source starts with a label and a Nop is inserted.
    assertx(start == 0 || start == 1);
  }
  m_sourceLocTab.push_back(std::make_pair(start, newLoc));
}


///////////////////////////////////////////////////////////////////////////////
// Mergeables.

void UnitEmitter::pushMergeableClass(PreClassEmitter* e) {
  m_mergeableStmts.push_back(std::make_pair(MergeKind::Class, e->id()));
}

void UnitEmitter::pushMergeableInclude(Unit::MergeKind kind,
                                       const StringData* unitName) {
  m_mergeableStmts.push_back(
    std::make_pair(kind, mergeLitstr(unitName)));
  m_allClassesHoistable = false;
}

void UnitEmitter::insertMergeableInclude(int ix, Unit::MergeKind kind, Id id) {
  assertx(size_t(ix) <= m_mergeableStmts.size());
  m_mergeableStmts.insert(m_mergeableStmts.begin() + ix,
                          std::make_pair(kind, id));
  m_allClassesHoistable = false;
}

void UnitEmitter::pushMergeableDef(Unit::MergeKind kind,
                                   const StringData* name,
                                   const TypedValue& tv) {
  m_mergeableStmts.push_back(std::make_pair(kind, m_mergeableValues.size()));
  m_mergeableValues.push_back(std::make_pair(mergeLitstr(name), tv));
  m_allClassesHoistable = false;
}

void UnitEmitter::insertMergeableDef(int ix, Unit::MergeKind kind,
                                     Id id, const TypedValue& tv) {
  assertx(size_t(ix) <= m_mergeableStmts.size());
  m_mergeableStmts.insert(m_mergeableStmts.begin() + ix,
                          std::make_pair(kind, m_mergeableValues.size()));
  m_mergeableValues.push_back(std::make_pair(id, tv));
  m_allClassesHoistable = false;
}

void UnitEmitter::pushMergeableTypeAlias(Unit::MergeKind kind, const Id id) {
  m_mergeableStmts.push_back(std::make_pair(kind, id));
  m_allClassesHoistable = false;
}

void UnitEmitter::insertMergeableTypeAlias(int ix, Unit::MergeKind kind,
                                           const Id id) {
  assertx(size_t(ix) <= m_mergeableStmts.size());
  m_mergeableStmts.insert(m_mergeableStmts.begin() + ix,
                          std::make_pair(kind, id));
  m_allClassesHoistable = false;
}

///////////////////////////////////////////////////////////////////////////////
// Initialization and execution.

void UnitEmitter::commit(UnitOrigin unitOrigin) {
  Repo& repo = Repo::get();
  try {
    RepoTxn txn(repo);
    RepoStatus err = insert(unitOrigin, txn);
    if (err == RepoStatus::success) {
      txn.commit();
    }
  } catch (RepoExc& re) {
    int repoId = repo.repoIdForNewUnit(unitOrigin);
    if (repoId != RepoIdInvalid) {
      TRACE(3, "Failed to commit '%s' (0x%016" PRIx64 "%016" PRIx64 ") to '%s': %s\n",
               m_filepath->data(), m_md5.q[0], m_md5.q[1],
               repo.repoName(repoId).c_str(), re.msg().c_str());
    }
  }
}

RepoStatus UnitEmitter::insert(UnitOrigin unitOrigin, RepoTxn& txn) {
  Repo& repo = Repo::get();
  UnitRepoProxy& urp = repo.urp();
  int repoId = Repo::get().repoIdForNewUnit(unitOrigin);
  if (repoId == RepoIdInvalid) {
    return RepoStatus::error;
  }
  m_repoId = repoId;

  try {
    {
      if (!m_sourceLocTab.empty()) {
        m_lineTable = createLineTable(m_sourceLocTab, m_bclen);
      }
      urp.insertUnit[repoId].insert(*this, txn, m_sn, m_md5, m_bc,
                                    m_bclen);
    }
    int64_t usn = m_sn;
    urp.insertUnitLineTable(repoId, txn, usn, m_lineTable);
    for (unsigned i = 0; i < m_litstrs.size(); ++i) {
      urp.insertUnitLitstr[repoId].insert(txn, usn, i, m_litstrs[i]);
    }
    for (unsigned i = 0; i < m_arrays.size(); ++i) {
      urp.insertUnitArray[repoId].insert(
        txn, usn, i,
        internal_serialize(
          VarNR(const_cast<ArrayData*>(m_arrays[i]))
        ).toCppString()
      );
    }
    urp.insertUnitArrayTypeTable[repoId].insert(txn, usn, m_arrayTypeTable);
    for (auto& fe : m_fes) {
      fe->commit(txn);
    }
    for (auto& pce : m_pceVec) {
      pce->commit(txn);
    }

    for (int i = 0, n = m_mergeableStmts.size(); i < n; i++) {
      switch (m_mergeableStmts[i].first) {
        case MergeKind::Done:
        case MergeKind::UniqueDefinedClass:
          not_reached();
        case MergeKind::Class: break;
        case MergeKind::TypeAlias:
        case MergeKind::ReqDoc: {
          urp.insertUnitMergeable[repoId].insert(
            txn, usn, i,
            m_mergeableStmts[i].first, m_mergeableStmts[i].second, nullptr);
          break;
        }
        case MergeKind::Define:
        case MergeKind::PersistentDefine:
        case MergeKind::Global: {
          int ix = m_mergeableStmts[i].second;
          urp.insertUnitMergeable[repoId].insert(
            txn, usn, i,
            m_mergeableStmts[i].first,
            m_mergeableValues[ix].first, &m_mergeableValues[ix].second);
          break;
        }
      }
    }
    if (RuntimeOption::RepoDebugInfo) {
      for (size_t i = 0; i < m_sourceLocTab.size(); ++i) {
        SourceLoc& e = m_sourceLocTab[i].second;
        Offset endOff = i < m_sourceLocTab.size() - 1
                            ? m_sourceLocTab[i + 1].first
                            : m_bclen;

        urp.insertUnitSourceLoc[repoId]
           .insert(txn, usn, endOff, e.line0, e.char0, e.line1, e.char1);
      }
    }
    return RepoStatus::success;
  } catch (RepoExc& re) {
    TRACE(3, "Failed to commit '%s' (0x%016" PRIx64 "%016" PRIx64 ") to '%s': %s\n",
             m_filepath->data(), m_md5.q[0], m_md5.q[1],
             repo.repoName(repoId).c_str(), re.msg().c_str());
    return RepoStatus::error;
  }
}

ServiceData::ExportedTimeSeries* g_hhbc_size = ServiceData::createTimeSeries(
  "vm.hhbc-size",
  {ServiceData::StatsType::AVG,
   ServiceData::StatsType::SUM,
   ServiceData::StatsType::COUNT}
);

static const unsigned char*
allocateBCRegion(const unsigned char* bc, size_t bclen) {
  g_hhbc_size->addValue(bclen);
  if (RuntimeOption::RepoAuthoritative) {
    // In RepoAuthoritative, we assume we won't ever deallocate units
    // and that this is read-only, mostly cold data.  So we throw it
    // in a bump-allocator that's mprotect'd to prevent writes.
    return static_cast<const unsigned char*>(
      get_readonly_arena().allocate(bc, bclen)
    );
  }
  auto mem = static_cast<unsigned char*>(malloc(bclen));
  std::copy(bc, bc + bclen, mem);
  return mem;
}

bool UnitEmitter::check(bool verbose) const {
  return Verifier::checkUnit(
    this,
    verbose ? Verifier::kVerbose : Verifier::kStderr
  );
}

std::unique_ptr<Unit> UnitEmitter::create(bool saveLineTable) const {
  INC_TPC(unit_load);

  static const bool kVerify = debug || RuntimeOption::EvalVerify ||
    RuntimeOption::EvalVerifyOnly || RuntimeOption::EvalFatalOnVerifyError;
  static const bool kVerifyVerboseSystem =
    getenv("HHVM_VERIFY_VERBOSE_SYSTEM");
  static const bool kVerifyVerbose =
    kVerifyVerboseSystem || getenv("HHVM_VERIFY_VERBOSE");

  const bool isSystemLib = FileUtil::isSystemName(m_filepath->slice());
  const bool doVerify =
    kVerify || boost::ends_with(m_filepath->data(), ".hhas");
  if (doVerify) {
    auto const verbose = isSystemLib ? kVerifyVerboseSystem : kVerifyVerbose;
    if (!check(verbose)) {
      if (!verbose) {
        std::cerr << folly::format(
          "Verification failed for unit {}. Re-run with "
          "HHVM_VERIFY_VERBOSE{}=1 to see more details.\n",
          m_filepath->data(), isSystemLib ? "_SYSTEM" : ""
        );
      }
      if (!RuntimeOption::EvalVerifyOnly &&
          RuntimeOption::EvalFatalOnVerifyError) {
        return createFatalUnit(
          const_cast<StringData*>(m_filepath),
          m_md5,
          FatalOp::Parse,
          makeStaticString("A bytecode verification error was detected")
        )->create(saveLineTable);
      }
    }
    if (!isSystemLib && RuntimeOption::EvalVerifyOnly) {
      std::fflush(stdout);
      _Exit(0);
    }
  }

  std::unique_ptr<Unit> u {
    RuntimeOption::RepoAuthoritative && !RuntimeOption::SandboxMode &&
      m_litstrs.empty() && m_arrayTypeTable.empty() ?
    new Unit : new UnitExtended
  };

  u->m_repoId = saveLineTable ? RepoIdInvalid : m_repoId;
  u->m_sn = m_sn;
  u->m_bc = allocateBCRegion(m_bc, m_bclen);
  u->m_bclen = m_bclen;
  u->m_filepath = m_filepath;
  u->m_mainReturn = m_mainReturn;
  u->m_mergeOnly = m_mergeOnly;
  u->m_isHHFile = m_isHHFile;
  u->m_useStrictTypes = m_useStrictTypes;
  u->m_useStrictTypesForBuiltins = m_useStrictTypesForBuiltins;
  u->m_dirpath = makeStaticString(FileUtil::dirname(StrNR{m_filepath}));
  u->m_md5 = m_md5;
  u->m_arrays = m_arrays;
  for (auto const& pce : m_pceVec) {
    u->m_preClasses.push_back(PreClassPtr(pce->create(*u)));
  }
  u->m_typeAliases = m_typeAliases;
  u->m_metaData = m_metaData;
  u->m_ICE = m_ICE;

  size_t ix = m_fes.size() + m_hoistablePceIdList.size();
  if (m_mergeOnly && !m_allClassesHoistable) {
    size_t extra = 0;
    for (auto& mergeable : m_mergeableStmts) {
      extra++;
      if (!RuntimeOption::RepoAuthoritative && SystemLib::s_inited) {
        if (mergeable.first != MergeKind::Class) {
          extra = 0;
          u->m_mergeOnly = false;
          break;
        }
      } else {
        switch (mergeable.first) {
          case MergeKind::PersistentDefine:
          case MergeKind::Define:
          case MergeKind::Global:
            extra += sizeof(TypedValueAux) / sizeof(void*);
            break;
          default:
            break;
        }
      }
    }
    ix += extra;
  }
  Unit::MergeInfo *mi = Unit::MergeInfo::alloc(ix);
  u->m_mergeInfo.store(mi, std::memory_order_relaxed);
  ix = 0;
  for (auto& fe : m_fes) {
    auto const func = fe->create(*u);
    if (func->top()) {
      if (!mi->m_firstHoistableFunc) {
        mi->m_firstHoistableFunc = ix;
      }
    } else {
      assertx(!mi->m_firstHoistableFunc);
    }
    assertx(ix == fe->id());
    mi->mergeableObj(ix++) = func;
  }
  assertx(u->getMain(nullptr)->isPseudoMain());
  if (!mi->m_firstHoistableFunc) {
    mi->m_firstHoistableFunc =  ix;
  }
  mi->m_firstHoistablePreClass = ix;
  assertx(m_fes.size());
  for (auto& id : m_hoistablePceIdList) {
    mi->mergeableObj(ix++) = u->m_preClasses[id].get();
  }
  mi->m_firstMergeablePreClass = ix;
  if (u->m_mergeOnly && !m_allClassesHoistable) {
    for (auto& mergeable : m_mergeableStmts) {
      switch (mergeable.first) {
        case MergeKind::Class:
          mi->mergeableObj(ix++) = u->m_preClasses[mergeable.second].get();
          break;
        case MergeKind::TypeAlias:
          mi->mergeableObj(ix++) =
            (void*)((intptr_t(mergeable.second) << 3) + (int)mergeable.first);
          break;
        case MergeKind::ReqDoc: {
          assertx(RuntimeOption::RepoAuthoritative);
          void* name = u->lookupLitstrId(mergeable.second);
          mi->mergeableObj(ix++) = (char*)name + (int)mergeable.first;
          break;
        }
        case MergeKind::Define:
        case MergeKind::Global:
          assertx(RuntimeOption::RepoAuthoritative);
        case MergeKind::PersistentDefine: {
          void* name = u->lookupLitstrId
            (m_mergeableValues[mergeable.second].first);
          mi->mergeableObj(ix++) = (char*)name + (int)mergeable.first;
          auto& tv = m_mergeableValues[mergeable.second].second;
          auto* tva = (TypedValueAux*)mi->mergeableData(ix);
          tva->m_data = tv.m_data;
          tva->m_type = tv.m_type;
          // leave tva->m_aux uninitialized
          ix += sizeof(*tva) / sizeof(void*);
          assertx(sizeof(*tva) % sizeof(void*) == 0);
          break;
        }
        case MergeKind::Done:
        case MergeKind::UniqueDefinedClass:
          not_reached();
      }
    }
  }
  assertx(ix == mi->m_mergeablesSize);
  mi->mergeableObj(ix) = (void*)MergeKind::Done;

  /*
   * What's going on is we're going to have a m_lineTable if this UnitEmitter
   * was loaded from the repo, and no m_sourceLocTab (it's demand-loaded by
   * unit.cpp because it's only used for the debugger).  Don't bother creating
   * the line table here, because we can retrieve it from the repo later.
   *
   * On the other hand, if this unit was just created by parsing a php file (or
   * whatnot) which was not committed to the repo, we'll have a m_sourceLocTab.
   * In this case we should populate m_lineTable (otherwise we might lose line
   * info altogether, since it may not be backed by a repo).
   */
  if (m_sourceLocTab.size() != 0) {
    stashLineTable(u.get(), createLineTable(m_sourceLocTab, m_bclen));
    // If we plan to dump hhas we will need the extended line table information
    // in the output, and if we're not writing the repo, stashing it here is
    // necessary for it to make it through.
    if (RuntimeOption::RepoDebugInfo &&
        RuntimeOption::EvalDumpHhas &&
        SystemLib::s_inited) {
      stashExtendedLineTable(u.get(), createSourceLocTable());
    }
  } else if (saveLineTable) {
    stashLineTable(u.get(), m_lineTable);
  }

  if (u->m_extended) {
    auto ux = u->getExtended();
    for (auto s : m_litstrs) {
      ux->m_namedInfo.push_back(s);
    }
    ux->m_arrayTypeTable = m_arrayTypeTable;

    // Funcs can be recorded out of order when loading them from the
    // repo currently.  So sort 'em here.
    std::sort(ux->m_funcTable.begin(), ux->m_funcTable.end(),
              [] (const Func* a, const Func* b) {
                return a->past() < b->past();
              });
  } else {
    assertx(!m_litstrs.size());
    assertx(m_arrayTypeTable.empty());
  }

  if (RuntimeOption::EvalDumpHhas > 1 ||
    (SystemLib::s_inited && RuntimeOption::EvalDumpHhas == 1)) {
    auto const& hhaspath = RuntimeOption::EvalDumpHhasToFile;
    if (!hhaspath.empty()) {
      static std::atomic<bool> first_unit{true};
      auto const flags = O_WRONLY | O_CREAT | (first_unit ? O_TRUNC : O_APPEND);
      if (!folly::writeFile(disassemble(u.get()), hhaspath.c_str(), flags)) {
        Logger::Error("Failed to write hhas to %s", hhaspath.c_str());
        _Exit(1);
      }
      first_unit = false;
    } else {
      std::printf("%s", disassemble(u.get()).c_str());
      std::fflush(stdout);
    }
    if (SystemLib::s_inited) {
      _Exit(0);
    }
  }

  if (RuntimeOption::EvalDumpBytecode) {
    // Dump human-readable bytecode.
    Trace::traceRelease("%s", u->toString().c_str());
  }

  return u;
}

template<class SerDe>
void UnitEmitter::serdeMetaData(SerDe& sd) {
  sd(m_mainReturn)
    (m_mergeOnly)
    (m_isHHFile)
    (m_typeAliases)
    (m_useStrictTypes)
    (m_useStrictTypesForBuiltins)
    (m_metaData)
    ;

  if (RuntimeOption::EvalLoadFilepathFromUnitCache) {
    /* May be different than the unit origin: e.g. for hhas files. */
    sd(m_filepath);
  }
}


///////////////////////////////////////////////////////////////////////////////
// UnitRepoProxy.

UnitRepoProxy::UnitRepoProxy(Repo& repo)
    : RepoProxy(repo)
#define URP_OP(c, o) \
    , o{c##Stmt(repo, 0), c##Stmt(repo, 1)}
    URP_OPS
#undef URP_OP
{}

UnitRepoProxy::~UnitRepoProxy() {
}

void UnitRepoProxy::createSchema(int repoId, RepoTxn& txn) {
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} "
      "(unitSn INTEGER PRIMARY KEY, md5 BLOB UNIQUE, preload INTEGER, bc BLOB, "
      " data BLOB);",
      m_repo.table(repoId, "Unit"));
    txn.exec(createQuery);
  }
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} "
      "(unitSn INTEGER, litstrId INTEGER, litstr TEXT,"
      " PRIMARY KEY (unitSn, litstrId));",
      m_repo.table(repoId, "UnitLitstr"));
    txn.exec(createQuery);
  }
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} "
      "(unitSn INTEGER, arrayId INTEGER, array BLOB,"
      " PRIMARY KEY (unitSn, arrayId));",
      m_repo.table(repoId, "UnitArray"));
    txn.exec(createQuery);
  }
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} "
      "(unitSn INTEGER PRIMARY KEY, arrayTypeTable BLOB);",
      m_repo.table(repoId, "UnitArrayTypeTable"));
    txn.exec(createQuery);
  }
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} "
      "(unitSn INTEGER, mergeableIx INTEGER, mergeableKind INTEGER, "
      " mergeableId INTEGER, mergeableValue BLOB, "
      " PRIMARY KEY (unitSn, mergeableIx));",
      m_repo.table(repoId, "UnitMergeables"));
    txn.exec(createQuery);
  }
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} "
      "(unitSn INTEGER, pastOffset INTEGER, line0 INTEGER,"
      " char0 INTEGER, line1 INTEGER, char1 INTEGER,"
      " PRIMARY KEY (unitSn, pastOffset));",
      m_repo.table(repoId, "UnitSourceLoc"));
    txn.exec(createQuery);
  }
  {
    auto createQuery = folly::sformat(
      "CREATE TABLE {} (unitSn INTEGER PRIMARY KEY, data BLOB);",
      m_repo.table(repoId, "UnitLineTable"));
    txn.exec(createQuery);
  }
}

RepoStatus UnitRepoProxy::loadHelper(UnitEmitter& ue,
                                     const std::string& name,
                                     const MD5& md5) {
  if (!RuntimeOption::EvalLoadFilepathFromUnitCache) {
    ue.m_filepath = makeStaticString(name);
  }
  // Look for a repo that contains a unit with matching MD5.
  int repoId;
  for (repoId = RepoIdCount - 1; repoId >= 0; --repoId) {
    if (getUnit[repoId].get(ue, md5) == RepoStatus::success) {
      break;
    }
  }
  if (repoId < 0) {
    TRACE(3, "No repo contains '%s' (0x%016" PRIx64  "%016" PRIx64 ")\n",
             name.c_str(), md5.q[0], md5.q[1]);
    return RepoStatus::error;
  }
  try {
    getUnitLitstrs[repoId].get(ue);
    getUnitArrays[repoId].get(ue);
    getUnitArrayTypeTable[repoId].get(ue);
    m_repo.pcrp().getPreClasses[repoId].get(ue);
    getUnitMergeables[repoId].get(ue);
    getUnitLineTable(repoId, ue.m_sn, ue.m_lineTable);
    m_repo.frp().getFuncs[repoId].get(ue);
  } catch (RepoExc& re) {
    TRACE(0,
          "Repo error loading '%s' (0x%016" PRIx64 "%016"
          PRIx64 ") from '%s': %s\n",
          name.c_str(), md5.q[0], md5.q[1], m_repo.repoName(repoId).c_str(),
          re.msg().c_str());
    return RepoStatus::error;
  }
  TRACE(3, "Repo loaded '%s' (0x%016" PRIx64 "%016" PRIx64 ") from '%s'\n",
           name.c_str(), md5.q[0], md5.q[1], m_repo.repoName(repoId).c_str());
  return RepoStatus::success;
}

std::unique_ptr<UnitEmitter>
UnitRepoProxy::loadEmitter(const std::string& name, const MD5& md5,
                           const Native::FuncTable& nativeFuncs) {
  auto ue = std::make_unique<UnitEmitter>(md5, nativeFuncs);
  if (loadHelper(*ue, name, md5) == RepoStatus::error) ue.reset();
  return ue;
}

std::unique_ptr<Unit>
UnitRepoProxy::load(const std::string& name, const MD5& md5,
                    const Native::FuncTable& nativeFuncs) {
  auto ue = std::make_unique<UnitEmitter>(md5, nativeFuncs);
  if (loadHelper(*ue, name, md5) == RepoStatus::error) return nullptr;

#ifdef USE_JEMALLOC
  if (RuntimeOption::TrackPerUnitMemory) {
    size_t len = sizeof(uint64_t*);
    uint64_t* alloc;
    uint64_t* del;
    mallctl("thread.allocatedp", static_cast<void*>(&alloc), &len, nullptr, 0);
    mallctl("thread.deallocatedp", static_cast<void*>(&del), &len, nullptr, 0);
    auto before = *alloc;
    auto debefore = *del;
    std::unique_ptr<Unit> result = ue->create();
    auto after = *alloc;
    auto deafter = *del;

    auto path = folly::sformat("/tmp/units-{}.map", getpid());
    auto change = (after - deafter) - (before - debefore);
    auto str = folly::sformat("{} {}\n", name, change);
    auto out = std::fopen(path.c_str(), "a");
    if (out) {
      std::fwrite(str.data(), str.size(), 1, out);
      std::fclose(out);
    }

    return result;
  }
#endif

  auto unit = ue->create();
  if (BuiltinSymbols::s_systemAr) {
    assertx(ue->m_filepath->data()[0] == '/' &&
            ue->m_filepath->data()[1] == ':');
    BuiltinSymbols::s_systemAr->addHhasFile(std::move(ue));
  }
  FTRACE(1, "Creating unit {} for `{}`\n", unit.get(), name);
  return unit;
}

void UnitRepoProxy::InsertUnitStmt
                  ::insert(const UnitEmitter& ue,
                           RepoTxn& txn, int64_t& unitSn, const MD5& md5,
                           const unsigned char* bc, size_t bclen) {
  BlobEncoder dataBlob;

  if (!prepared()) {
    /*
     * Do not put preload into data; its needed to choose the
     * units in preloadRepo.
     */
    auto insertQuery = folly::sformat(
      "INSERT INTO {} VALUES(NULL, @md5, @preload, @bc, @data);",
      m_repo.table(m_repoId, "Unit"));
    txn.prepare(*this, insertQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindMd5("@md5", md5);
  query.bindInt("@preload", ue.m_preloadPriority);
  query.bindBlob("@bc", (const void*)bc, bclen);
  const_cast<UnitEmitter&>(ue).serdeMetaData(dataBlob);
  query.bindBlob("@data", dataBlob, /* static */ true);
  query.exec();
  unitSn = query.getInsertedRowid();
}

RepoStatus UnitRepoProxy::GetUnitStmt::get(UnitEmitter& ue, const MD5& md5) {
  try {
    RepoTxn txn(m_repo);
    if (!prepared()) {
      auto selectQuery = folly::sformat(
        "SELECT unitSn, preload, bc, data FROM {} WHERE md5 == @md5;",
        m_repo.table(m_repoId, "Unit"));
      txn.prepare(*this, selectQuery);
    }
    RepoTxnQuery query(txn, *this);
    query.bindMd5("@md5", md5);
    query.step();
    if (!query.row()) {
      return RepoStatus::error;
    }
    int64_t unitSn;                     /**/ query.getInt64(0, unitSn);
    int preloadPriority;                /**/ query.getInt(1, preloadPriority);
    const void* bc; size_t bclen;       /**/ query.getBlob(2, bc, bclen);
    BlobDecoder dataBlob =              /**/ query.getBlob(3);

    ue.m_repoId = m_repoId;
    ue.m_sn = unitSn;
    ue.m_preloadPriority = preloadPriority;
    ue.setBc(static_cast<const unsigned char*>(bc), bclen);
    ue.serdeMetaData(dataBlob);

    txn.commit();
  } catch (RepoExc& re) {
    return RepoStatus::error;
  }
  return RepoStatus::success;
}

void UnitRepoProxy::InsertUnitLitstrStmt
                  ::insert(RepoTxn& txn, int64_t unitSn, Id litstrId,
                           const StringData* litstr) {
  if (!prepared()) {
    auto insertQuery = folly::sformat(
      "INSERT INTO {} VALUES(@unitSn, @litstrId, @litstr);",
      m_repo.table(m_repoId, "UnitLitstr"));
    txn.prepare(*this, insertQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", unitSn);
  query.bindId("@litstrId", litstrId);
  query.bindStaticString("@litstr", litstr);
  query.exec();
}

void UnitRepoProxy::GetUnitLitstrsStmt
                  ::get(UnitEmitter& ue) {
  RepoTxn txn(m_repo);
  if (!prepared()) {
    auto selectQuery = folly::sformat(
      "SELECT litstrId, litstr FROM {} "
      " WHERE unitSn == @unitSn ORDER BY litstrId ASC;",
      m_repo.table(m_repoId, "UnitLitstr"));
    txn.prepare(*this, selectQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", ue.m_sn);
  do {
    query.step();
    if (query.row()) {
      Id litstrId;        /**/ query.getId(0, litstrId);
      StringData* litstr; /**/ query.getStaticString(1, litstr);
      Id id UNUSED = ue.mergeUnitLitstr(litstr);
      assertx(id == litstrId);
    }
  } while (!query.done());
  txn.commit();
}

void UnitRepoProxy::InsertUnitArrayTypeTableStmt::insert(
  RepoTxn& txn, int64_t unitSn, const ArrayTypeTable& att) {

  if (!prepared()) {
    auto insertQuery = folly::sformat(
      "INSERT INTO {} VALUES(@unitSn, @arrayTypeTable);",
      m_repo.table(m_repoId, "UnitArrayTypeTable"));
    txn.prepare(*this, insertQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", unitSn);
  BlobEncoder dataBlob;
  dataBlob(att);
  query.bindBlob("@arrayTypeTable", dataBlob, /* static */ true);
  query.exec();
}

void UnitRepoProxy::GetUnitArrayTypeTableStmt
                  ::get(UnitEmitter& ue) {
  RepoTxn txn(m_repo);
  if (!prepared()) {
    auto selectQuery = folly::sformat(
      "SELECT unitSn, arrayTypeTable FROM {} WHERE unitSn == @unitSn;",
      m_repo.table(m_repoId, "UnitArrayTypeTable"));
    txn.prepare(*this, selectQuery);
  }

  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", ue.m_sn);

  query.step();
  assertx(query.row());
  BlobDecoder dataBlob = query.getBlob(1);
  dataBlob(ue.m_arrayTypeTable);
  dataBlob.assertDone();
  query.step();
  assertx(query.done());

  txn.commit();
}

void UnitRepoProxy::InsertUnitArrayStmt
                  ::insert(RepoTxn& txn, int64_t unitSn, Id arrayId,
                           const std::string& array) {
  if (!prepared()) {
    auto insertQuery = folly::sformat(
      "INSERT INTO {} VALUES(@unitSn, @arrayId, @array);",
      m_repo.table(m_repoId, "UnitArray"));
    txn.prepare(*this, insertQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", unitSn);
  query.bindId("@arrayId", arrayId);
  query.bindStdString("@array", array);
  query.exec();
}

void UnitRepoProxy::GetUnitArraysStmt
                  ::get(UnitEmitter& ue) {
  RepoTxn txn(m_repo);
  if (!prepared()) {
    auto selectQuery = folly::sformat(
      "SELECT arrayId, array FROM {} "
      " WHERE unitSn == @unitSn ORDER BY arrayId ASC;",
      m_repo.table(m_repoId, "UnitArray"));
    txn.prepare(*this, selectQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", ue.m_sn);
  do {
    query.step();
    if (query.row()) {
      Id arrayId;        /**/ query.getId(0, arrayId);
      std::string key;   /**/ query.getStdString(1, key);
      Variant v = unserialize_from_buffer(
        key.data(),
        key.size(),
        VariableUnserializer::Type::Internal
      );
      Id id DEBUG_ONLY = ue.mergeArray(ArrayData::GetScalarArray(std::move(v)));
      assertx(id == arrayId);
    }
  } while (!query.done());
  txn.commit();
}

void UnitRepoProxy::InsertUnitMergeableStmt
                  ::insert(RepoTxn& txn, int64_t unitSn,
                           int ix, Unit::MergeKind kind, Id id,
                           TypedValue* value) {
  if (!prepared()) {
    auto insertQuery = folly::sformat(
      "INSERT INTO {} VALUES("
      " @unitSn, @mergeableIx, @mergeableKind, @mergeableId, @mergeableValue);",
      m_repo.table(m_repoId, "UnitMergeables"));
    txn.prepare(*this, insertQuery);
  }

  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", unitSn);
  query.bindInt("@mergeableIx", ix);
  query.bindInt("@mergeableKind", (int)kind);
  query.bindId("@mergeableId", id);
  if (value) {
    assertx(kind == MergeKind::Define ||
           kind == MergeKind::PersistentDefine ||
           kind == MergeKind::Global);
    query.bindTypedValue("@mergeableValue", *value);
  } else {
    assertx(kind == MergeKind::ReqDoc || kind == MergeKind::TypeAlias);
    query.bindNull("@mergeableValue");
  }
  query.exec();
}

void UnitRepoProxy::GetUnitMergeablesStmt
                  ::get(UnitEmitter& ue) {
  RepoTxn txn(m_repo);
  if (!prepared()) {
    auto selectQuery = folly::sformat(
      "SELECT mergeableIx, mergeableKind, mergeableId, mergeableValue "
      "FROM {} "
      "WHERE unitSn == @unitSn ORDER BY mergeableIx ASC;",
      m_repo.table(m_repoId, "UnitMergeables"));
    txn.prepare(*this, selectQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", ue.m_sn);
  do {
    query.step();
    if (query.row()) {
      int mergeableIx;           /**/ query.getInt(0, mergeableIx);
      int mergeableKind;         /**/ query.getInt(1, mergeableKind);
      Id mergeableId;            /**/ query.getInt(2, mergeableId);

      auto k = MergeKind(mergeableKind);

      if (UNLIKELY(!RuntimeOption::RepoAuthoritative)) {
        /*
         * We're using a repo generated in WholeProgram mode,
         * but we're not using it in RepoAuthoritative mode
         * (this is dodgy to start with). We're not going to
         * deal with requires at merge time, so drop them
         * here, and clear the mergeOnly flag for the unit.
         * The two exceptions are persistent constants and
         * TypeAliases which are allowed in systemlib.
         */
        if ((k != MergeKind::PersistentDefine && k != MergeKind::TypeAlias)
            || SystemLib::s_inited) {
          ue.m_mergeOnly = false;
        }
      }
      switch (k) {
        case MergeKind::TypeAlias:
          ue.insertMergeableTypeAlias(mergeableIx, k, mergeableId);
          break;
        case MergeKind::ReqDoc:
          ue.insertMergeableInclude(mergeableIx, k, mergeableId);
          break;
        case MergeKind::PersistentDefine:
        case MergeKind::Define:
        case MergeKind::Global: {
          TypedValue mergeableValue; /**/ query.getTypedValue(3,
                                                              mergeableValue);
          ue.insertMergeableDef(mergeableIx, k, mergeableId, mergeableValue);
          break;
        }
        default: break;
      }
    }
  } while (!query.done());
  txn.commit();
}

void UnitRepoProxy::insertUnitLineTable(int repoId,
                                        RepoTxn& txn,
                                        int64_t unitSn,
                                        LineTable& lineTable) {
  RepoStmt stmt(m_repo);
  stmt.prepare(
    folly::format(
      "INSERT INTO {} VALUES(@unitSn, @data);",
      m_repo.table(repoId, "UnitLineTable")
    ).str());

  RepoTxnQuery query(txn, stmt);
  BlobEncoder dataBlob;
  dataBlob.encode(lineTable);
  query.bindInt64("@unitSn", unitSn);
  query.bindBlob("@data", dataBlob, /* static */ true);
  query.exec();
}

void UnitRepoProxy::getUnitLineTable(int repoId,
                                     int64_t unitSn,
                                     LineTable& lineTable) {
  RepoStmt stmt(m_repo);
  stmt.prepare(
    folly::format(
      "SELECT data FROM {} WHERE unitSn == @unitSn;",
      m_repo.table(repoId, "UnitLineTable")
    ).str());

  RepoTxn txn(m_repo);
  RepoTxnQuery query(txn, stmt);
  query.bindInt64("@unitSn", unitSn);
  query.step();
  if (query.row()) {
    BlobDecoder dataBlob = query.getBlob(0);
    dataBlob.decode(lineTable);
  }
  txn.commit();
}

void UnitRepoProxy::InsertUnitSourceLocStmt
                  ::insert(RepoTxn& txn, int64_t unitSn, Offset pastOffset,
                           int line0, int char0, int line1, int char1) {
  if (!prepared()) {
    auto insertQuery = folly::sformat(
      "INSERT INTO {} "
      "VALUES(@unitSn, @pastOffset, @line0, @char0, @line1, @char1);",
      m_repo.table(m_repoId, "UnitSourceLoc"));
    txn.prepare(*this, insertQuery);
  }
  RepoTxnQuery query(txn, *this);
  query.bindInt64("@unitSn", unitSn);
  query.bindOffset("@pastOffset", pastOffset);
  query.bindInt("@line0", line0);
  query.bindInt("@char0", char0);
  query.bindInt("@line1", line1);
  query.bindInt("@char1", char1);
  query.exec();
}

RepoStatus
UnitRepoProxy::GetSourceLocTabStmt::get(int64_t unitSn,
                                        SourceLocTable& sourceLocTab) {
  try {
    RepoTxn txn(m_repo);
    if (!prepared()) {
      auto selectQuery = folly::sformat(
        "SELECT pastOffset, line0, char0, line1, char1 "
        "FROM {} "
        "WHERE unitSn == @unitSn "
        "ORDER BY pastOffset ASC;",
        m_repo.table(m_repoId, "UnitSourceLoc"));
      txn.prepare(*this, selectQuery);
    }
    RepoTxnQuery query(txn, *this);
    query.bindInt64("@unitSn", unitSn);
    do {
      query.step();
      if (!query.row()) {
        return RepoStatus::error;
      }
      Offset pastOffset;
      query.getOffset(0, pastOffset);
      SourceLoc sLoc;
      query.getInt(1, sLoc.line0);
      query.getInt(2, sLoc.char0);
      query.getInt(3, sLoc.line1);
      query.getInt(4, sLoc.char1);
      SourceLocEntry entry(pastOffset, sLoc);
      sourceLocTab.push_back(entry);
    } while (!query.done());
    txn.commit();
  } catch (RepoExc& re) {
    return RepoStatus::error;
  }
  return RepoStatus::success;
}

std::unique_ptr<UnitEmitter>
createFatalUnit(StringData* filename, const MD5& md5, FatalOp /*op*/,
                StringData* err) {
  auto ue = std::make_unique<UnitEmitter>(md5, Native::s_noNativeFuncs);
  ue->m_filepath = filename;
  ue->initMain(1, 1);
  ue->emitOp(OpString);
  ue->emitInt32(ue->mergeLitstr(err));
  ue->emitOp(OpFatal);
  ue->emitByte(static_cast<uint8_t>(FatalOp::Runtime));
  FuncEmitter* fe = ue->getMain();
  fe->maxStackCells = 1;
  // XXX line numbers are bogus
  fe->finish(ue->bcPos(), false);
  return ue;
}

///////////////////////////////////////////////////////////////////////////////
}
