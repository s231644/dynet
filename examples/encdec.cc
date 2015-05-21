#include "cnn/nodes.h"
#include "cnn/cnn.h"
#include "cnn/training.h"
#include "cnn/timing.h"
#include "cnn/rnn.h"
#include "cnn/lstm.h"
#include "cnn/dict.h"

#include <iostream>
#include <fstream>
#include <sstream>

#include <boost/archive/text_iarchive.hpp>
#include <boost/archive/text_oarchive.hpp>

using namespace std;
using namespace cnn;

unsigned LAYERS = 2;
unsigned INPUT_DIM = 8;
unsigned HIDDEN_DIM = 24;
unsigned INPUT_VOCAB_SIZE = 0;
unsigned OUTPUT_VOCAB_SIZE = 0;

cnn::Dict d;
int kSOS;
int kEOS;

template <class Builder>
struct EncoderDecoder {
  LookupParameters* p_c;
  LookupParameters* p_ec;  // map input to embedding (used in fwd and rev models)
  Parameters* p_ie2h;
  Parameters* p_bie;
  Parameters* p_h2oe;
  Parameters* p_boe;
  Parameters* p_R;
  Parameters* p_bias;
  Builder dec_builder;
  Builder rev_enc_builder;
  Builder fwd_enc_builder;
  explicit EncoderDecoder(Model& model) :
      dec_builder(LAYERS, INPUT_DIM, HIDDEN_DIM, &model),
      rev_enc_builder(LAYERS, INPUT_DIM, HIDDEN_DIM, &model),
      fwd_enc_builder(LAYERS, INPUT_DIM, HIDDEN_DIM, &model) {
    p_ie2h = model.add_parameters({long(HIDDEN_DIM * LAYERS * 1.5), long(HIDDEN_DIM * LAYERS * 2)});
    p_bie = model.add_parameters({long(HIDDEN_DIM * LAYERS * 1.5)});
    p_h2oe = model.add_parameters({long(HIDDEN_DIM * LAYERS), long(HIDDEN_DIM * LAYERS * 1.5)});
    p_boe = model.add_parameters({long(HIDDEN_DIM * LAYERS)});
    p_c = model.add_lookup_parameters(INPUT_VOCAB_SIZE, {INPUT_DIM}); 
    p_ec = model.add_lookup_parameters(INPUT_VOCAB_SIZE, {INPUT_DIM}); 
    p_R = model.add_parameters({OUTPUT_VOCAB_SIZE, HIDDEN_DIM});
    p_bias = model.add_parameters({OUTPUT_VOCAB_SIZE});
  }

  // build graph and return VariableIndex of total loss
  VariableIndex BuildGraph(const vector<int>& insent, const vector<int>& osent, ComputationGraph& cg) {
    // forward encoder
    fwd_enc_builder.new_graph(&cg);
    fwd_enc_builder.start_new_sequence();
    for (unsigned t = 0; t < insent.size(); ++t) {
      VariableIndex i_x_t = cg.add_lookup(p_ec, insent[t]);
      fwd_enc_builder.add_input(i_x_t, &cg);
    }
    // backward encoder
    rev_enc_builder.new_graph(&cg);
    rev_enc_builder.start_new_sequence();
    for (int t = insent.size() - 1; t >= 0; --t) {
      VariableIndex i_x_t = cg.add_lookup(p_ec, insent[t]);
      rev_enc_builder.add_input(i_x_t, &cg);
    }

    // encoder -> decoder transformation
    vector<VariableIndex> to(LAYERS * 2);
    int c = 0;
    for (auto h_l : fwd_enc_builder.final_h()) to[c++] = h_l;
    for (auto h_l : rev_enc_builder.final_h()) to[c++] = h_l;
    assert(c == LAYERS * 2);
    VariableIndex i_combined = cg.add_function<Concatenate>(to);
    VariableIndex i_ie2h = cg.add_parameters(p_ie2h);
    VariableIndex i_bie = cg.add_parameters(p_bie);
    VariableIndex i_t = cg.add_function<AffineTransform>({i_bie, i_ie2h, i_combined});
    cg.incremental_forward();
    VariableIndex i_h = cg.add_function<Rectify>({i_t});
    VariableIndex i_h2oe = cg.add_parameters(p_h2oe);
    VariableIndex i_boe = cg.add_parameters(p_boe);
    VariableIndex i_nc = cg.add_function<AffineTransform>({i_boe, i_h2oe, i_h});
    vector<VariableIndex> oein(LAYERS * 2);
    for (int i = 0; i < LAYERS; ++i) {
      oein[i] = cg.add_function<PickRange>({i_nc}, i * HIDDEN_DIM, (i + 1) * HIDDEN_DIM);
      oein[i + LAYERS] = cg.add_function<Tanh>({oein[i]});
    }
    dec_builder.new_graph(&cg);
    dec_builder.start_new_sequence(oein);

    // decoder
    VariableIndex i_R = cg.add_parameters(p_R);
    VariableIndex i_bias = cg.add_parameters(p_bias);
    vector<VariableIndex> errs;
    const unsigned oslen = osent.size() - 1;
    for (unsigned t = 0; t < oslen; ++t) {
      VariableIndex i_x_t = cg.add_lookup(p_c, osent[t]);
      VariableIndex i_y_t = dec_builder.add_input(i_x_t, &cg);
      VariableIndex i_r_t = cg.add_function<AffineTransform>({i_bias, i_R, i_y_t});
      VariableIndex i_ydist = cg.add_function<LogSoftmax>({i_r_t});
      errs.push_back(cg.add_function<PickElement>({i_ydist}, osent[t+1]));
    }
    VariableIndex i_nerr = cg.add_function<Sum>(errs);
    return cg.add_function<Negate>({i_nerr});
  }
};

int main(int argc, char** argv) {
  cnn::Initialize(argc, argv);
  if (argc != 3 && argc != 4) {
    cerr << "Usage: " << argv[0] << " corpus.txt dev.txt [model.params]\n";
    return 1;
  }
  kSOS = d.Convert("<s>");
  kEOS = d.Convert("</s>");
  vector<vector<int>> training, dev;
  string line;
  int tlc = 0;
  int ttoks = 0;
  cerr << "Reading training data from " << argv[1] << "...\n";
  {
    ifstream in(argv[1]);
    assert(in);
    while(getline(in, line)) {
      ++tlc;
      training.push_back(ReadSentence(line, &d));
      ttoks += training.back().size();
      if (training.back().front() != kSOS && training.back().back() != kEOS) {
        cerr << "Training sentence in " << argv[1] << ":" << tlc << " didn't start or end with <s>, </s>\n";
        abort();
      }
    }
    cerr << tlc << " lines, " << ttoks << " tokens, " << d.size() << " types\n";
  }
  d.Freeze(); // no new word types allowed
  INPUT_VOCAB_SIZE = d.size();
  OUTPUT_VOCAB_SIZE = d.size();

  int dlc = 0;
  int dtoks = 0;
  cerr << "Reading dev data from " << argv[2] << "...\n";
  {
    ifstream in(argv[2]);
    assert(in);
    while(getline(in, line)) {
      ++dlc;
      dev.push_back(ReadSentence(line, &d));
      dtoks += dev.back().size();
      if (dev.back().front() != kSOS && dev.back().back() != kEOS) {
        cerr << "Dev sentence in " << argv[2] << ":" << tlc << " didn't start or end with <s>, </s>\n";
        abort();
      }
    }
    cerr << dlc << " lines, " << dtoks << " tokens\n";
  }
  ostringstream os;
  os << "bilm"
     << '_' << LAYERS
     << '_' << INPUT_DIM
     << '_' << HIDDEN_DIM
     << "-pid" << getpid() << ".params";
  const string fname = os.str();
  cerr << "Parameters will be written to: " << fname << endl;
  double best = 9e+99;

  Model model;
  bool use_momentum = false;
  Trainer* sgd = nullptr;
  if (use_momentum)
    sgd = new MomentumSGDTrainer(&model);
  else
    sgd = new SimpleSGDTrainer(&model);

  //RNNBuilder rnn(LAYERS, INPUT_DIM, HIDDEN_DIM, &model);
  EncoderDecoder<LSTMBuilder> lm(model);
  if (argc == 4) {
    string fname = argv[3];
    ifstream in(fname);
    boost::archive::text_iarchive ia(in);
    ia >> model;
  }

  unsigned report_every_i = 50;
  unsigned dev_every_i_reports = 10;
  unsigned si = training.size();
  vector<unsigned> order(training.size());
  for (unsigned i = 0; i < order.size(); ++i) order[i] = i;
  bool first = true;
  int report = 0;
  unsigned lines = 0;
  while(1) {
    Timer iteration("completed in");
    double loss = 0;
    unsigned chars = 0;
    for (unsigned i = 0; i < report_every_i; ++i) {
      if (si == training.size()) {
        si = 0;
        if (first) { first = false; } else { sgd->update_epoch(); }
        cerr << "**SHUFFLE\n";
        random_shuffle(order.begin(), order.end());
      }

      // build graph for this instance
      ComputationGraph cg;
      auto& sent = training[order[si]];
      chars += sent.size() - 1;
      ++si;
      lm.BuildGraph(sent, sent, cg);
      loss += as_scalar(cg.forward());
      cg.backward();
      sgd->update();
      ++lines;
    }
    sgd->status();
    cerr << " E = " << (loss / chars) << " ppl=" << exp(loss / chars) << ' ';

#if 0
    lm.RandomSample();
#endif

    // show score on dev data?
    report++;
    if (report % dev_every_i_reports == 0) {
      double dloss = 0;
      int dchars = 0;
      for (auto& sent : dev) {
        ComputationGraph cg;
        lm.BuildGraph(sent, sent, cg);
        dloss += as_scalar(cg.forward());
        dchars += sent.size() - 1;
      }
      if (dloss < best) {
        best = dloss;
        ofstream out(fname);
        boost::archive::text_oarchive oa(out);
        oa << model;
      }
      cerr << "\n***DEV [epoch=" << (lines / (double)training.size()) << "] E = " << (dloss / dchars) << " ppl=" << exp(dloss / dchars) << ' ';
    }
  }
  delete sgd;
}

