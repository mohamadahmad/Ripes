#include <QDir>
#include <QProcess>
#include <QStringList>
#include <QtTest/QTest>

#include "processorhandler.h"
#include "processorregistry.h"

#ifndef VSRTL_RISCV_TEST_DIR
static_assert(false, "VSRTL_RISCV_TEST_DIR must be defined");
#endif

/** RISC-V test suite
 *
 * For now, the following assumptions are made:
 * - When compiling, it is assumed that the entry point address is 0x0
 * - No .data segment is contained within the resulting .ELF file
 * As such, we directly copy the .text segment into the simulator memory and execute the test.
 */

using namespace Ripes;
using namespace vsrtl::core;

// Compilation tools & directories
const QString s_testdir = VSRTL_RISCV_TEST_DIR;
const QString s_outdir = s_testdir + QDir::separator() + "build";
const QString s_assembler = "riscv64-unknown-elf-as";
const QString s_objcopy = "riscv64-unknown-elf-objcopy";
const QString s_linkerScript = "rvtest.ld";

// Ecall status codes
static constexpr unsigned s_success = 42;
static constexpr unsigned s_fail = 0;

// Test status register
static constexpr unsigned s_statusreg = 3;  // Current test stored in the gp(3) register
static constexpr unsigned s_ecallreg = 10;  // a0

// Maximum cycle count
static constexpr unsigned s_maxCycles = 10000;

// Tests which contains instructions or assembler directives not yet supported
const auto s_excludedTests = {"f", "ldst", "move", "recoding", /* fails on CI, unknown as of know */ "memory"};

QString compileTestFile(const QString& testfile) {
    QProcess exec;

    const QString outElf = s_outdir + QDir::separator() + testfile + ".out";
    const QString outBin = s_outdir + QDir::separator() + testfile + ".bin";

    if (!QDir(s_outdir).exists()) {
        QDir().mkdir(s_outdir);
    }

    // Build
    bool error = exec.execute(s_assembler, {"-march=rv32im", s_testdir + QDir::separator() + testfile, "-o", outElf});

    // Extract .text segment
    error = exec.execute(s_objcopy, {"-O", "binary", "--only-section=.text", outElf, outBin});

    return error ? QString() : outBin;
}

class tst_RISCV : public QObject {
    Q_OBJECT

private:
    void loadBinaryToSimulator(const QString& binFile);
    bool skipTest(const QString& test);
    QString executeSimulator();
    QString dumpRegs();

    QString m_currentTest;

    void runTests(const ProcessorID& id);

    void handleSysCall();

    bool m_stop = false;
    Program m_program;
    QString m_err;

private slots:

    void testRVSingleCycle() { runTests(ProcessorID::RVSS); }
    void testRV5StagePipeline() { runTests(ProcessorID::RV5S); }

    void cleanupTestCase();
};

void tst_RISCV::cleanupTestCase() {
    auto buildDir = QDir(s_outdir);
    buildDir.removeRecursively();
}

bool tst_RISCV::skipTest(const QString& test) {
    for (const auto& t : s_excludedTests) {
        if (test.startsWith(t)) {
            return true;
        }
    }
    return false;
}

QString tst_RISCV::dumpRegs() {
    QString str = "\n" + m_currentTest + "\nRegister dump:";
    str += "\t PC:" + QString::number(ProcessorHandler::get()->getProcessor()->getPcForStage(0), 16) + "\n";
    for (unsigned i = 0; i < ProcessorHandler::get()->currentISA()->regCnt(); i++) {
        str += "\t" + ProcessorHandler::get()->currentISA()->regName(i) + ":" +
               ProcessorHandler::get()->currentISA()->regAlias(i) + ":\t" +
               QString::number(ProcessorHandler::get()->getProcessor()->getRegister(i)) + "\n";
    }
    return str;
}

void tst_RISCV::loadBinaryToSimulator(const QString& binFile) {
    // Read test file
    QFile testFile(binFile);
    if (!testFile.open(QIODevice::ReadOnly)) {
        QString err = "Test: '" + m_currentTest + "' failed: Could not read compiled test file.";
        QFAIL(err.toStdString().c_str());
    }
    QByteArray programByteArray = testFile.readAll();

    m_program = Program();
    m_program.sections.push_back({TEXT_SECTION_NAME, 0, programByteArray});
    ProcessorHandler::get()->loadProgram(&m_program);
}

void tst_RISCV::handleSysCall() {
    unsigned status = ProcessorHandler::get()->getProcessor()->getRegister(s_ecallreg);
    if (status == s_success) {
        m_stop |= true;
    } else if (status == s_fail) {
        m_err = "Test: '" + m_currentTest + "' failed: Internal test error.\n\t test number: " +
                QString::number(ProcessorHandler::get()->getProcessor()->getRegister(s_statusreg));
        m_err += dumpRegs();
    }
}

QString tst_RISCV::executeSimulator() {
    m_stop = false;
    m_err = QString();
    bool maxCyclesReached = false;
    unsigned cycles = 0;
    do {
        ProcessorHandler::get()->getProcessorNonConst()->clock();

        cycles++;

        maxCyclesReached |= cycles >= s_maxCycles;
        m_stop |= maxCyclesReached;
    } while (!m_stop);

    if (maxCyclesReached) {
        m_err = "Test: '" + m_currentTest + "' failed: Maximum cycle count reached\n\t test number: " +
                QString::number(ProcessorHandler::get()->getProcessor()->getRegister(s_statusreg));
        m_err += dumpRegs();
    }

    // Test successful
    return m_err;
}

void tst_RISCV::runTests(const ProcessorID& id) {
    const auto dir = QDir(s_testdir);
    const auto testFiles = dir.entryList({"*.s"});

    for (const auto& test : testFiles) {
        m_currentTest = test;

        if (skipTest(m_currentTest))
            continue;

        qInfo() << "Running test: " << m_currentTest;

        // Compile test file
        const auto binFile = compileTestFile(test);
        if (binFile.isNull()) {
            QString err = "Test: '" + test + "' failed: Could not compile test file.";
            QFAIL(err.toStdString().c_str());
        }

        connect(ProcessorHandler::get(), &ProcessorHandler::reqReloadProgram, [=] { loadBinaryToSimulator(binFile); });
        connect(ProcessorHandler::get(), &ProcessorHandler::reqProcessorReset,
                [=] { ProcessorHandler::get()->getProcessorNonConst()->reset(); });

        ProcessorHandler::get()->selectProcessor(id);
        // Override the ProcessorHandler's ECALL handling
        ProcessorHandler::get()->getProcessorNonConst()->handleSysCall.Connect(this, &tst_RISCV::handleSysCall);

        const QString err = executeSimulator();
        if (!err.isNull()) {
            QFAIL(err.toStdString().c_str());
        }

        qInfo() << "Test '" << m_currentTest << "' succeeded.";
    }
}

QTEST_APPLESS_MAIN(tst_RISCV)
#include "tst_riscv.moc"
