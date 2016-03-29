#include "camera.hpp"
#include "persistence.hpp"
#include "math.hpp"
#include "positions.hpp"
#include "viables.hpp"
#include "rendering.hpp"
#include "graph_util.hpp"
#include <GLFW/glfw3.h>
#include <GL/glu.h>
#include <boost/program_options.hpp>
#include <iostream>
#include <vector>
#include <fstream>

using Frames = vector<pair<string, vector<Position>>>;

Frames smoothen(Frames f)
{
	Position last_pos = f[0].second[0];

	foreach (x : f)
	foreach (p : x.second)
	foreach (j : playerJoints)
	{
		double const lag = std::min(0.83, 0.6 + p[j].y);
		p[j] = last_pos[j] = last_pos[j] * lag + p[j] * (1 - lag);
	}

	return f;
}

Frames frames(Graph const & g, Path const & path, unsigned const frames_per_pos)
{
	if (path.empty()) return {};

	auto d = [&](SeqNum seq)
		{
			assert(!g[seq].description.empty());
			string desc = g[seq].description.front();
			if (desc == "..." && !g[g.to(seq).node].description.empty()) desc = g[g.to(seq).node].description.front();
			desc = replace_all(desc, "\\n", " ");
			return desc;
		};

	Frames r;
	ReorientedNode n = from(g, path.front());

	foreach (step : path)
	{
		pair<vector<Position>, ReorientedNode> p = follow(g, n, step.seq, frames_per_pos);

		p.first.pop_back();

		r.emplace_back(d(step.seq), p.first);

		n = p.second;
	}

	return r;
}

Frames frames(Graph const & g, vector<Path> const & script, unsigned const frames_per_pos)
{
	Frames full;
	foreach (path : script) append(full, frames(g, path, frames_per_pos));
	return full;
}

struct Config
{
	string db;
	string script;
	unsigned frames_per_pos;
	unsigned num_transitions;
	string start;
	optional<string /* desc */> demo;
	optional<pair<unsigned, unsigned>> dimensions;
};

optional<Config> config_from_args(int const argc, char const * const * const argv)
{
	namespace po = boost::program_options;

	po::options_description desc("options");
	desc.add_options()
		("help", "show this help")
		("frames-per-pos", po::value<unsigned>()->default_value(9),
			"number of frames rendered per position")
		("script", po::value<string>()->default_value(string()),
			"script file")
		("start", po::value<string>()->default_value("deep half"), "initial node (only used if no script given)")
		("length", po::value<unsigned>()->default_value(50), "number of transitions")
		("dimensions", po::value<string>(), "window dimensions")
		("db", po::value<string>()->default_value("GrappleMap.txt"), "database file")
		("demo", po::value<string>(), "show all chains of three transitions that have the given transition in the middle");

	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	po::notify(vm);

	if (vm.count("help")) { std::cout << desc << '\n'; return none; }

	optional<pair<unsigned, unsigned>> dimensions;
	if (vm.count("dimensions"))
	{
		string const dims = vm["dimensions"].as<string>();
		auto x = dims.find('x');
		if (x == dims.npos) throw runtime_error("invalid dimensions");
		dimensions = pair<unsigned, unsigned>(std::stoul(dims.substr(0, x)), std::stoul(dims.substr(x + 1)));
	}

	return Config
		{ vm["db"].as<string>()
		, vm["script"].as<string>()
		, vm["frames-per-pos"].as<unsigned>()
		, vm["length"].as<unsigned>()
		, vm["start"].as<string>()
		, vm.count("demo") ? optional<string>(vm["demo"].as<string>()) : boost::none
		, dimensions
		};
}

bool dfsScene(
	Graph const & g,
	vector<pair<vector<Step>, vector<Step>>> const & in_out,
	NodeNum const n, size_t const size, Path & scene)
{
	if (size == 0) return true;

	std::multimap<size_t, Step> choices;

	foreach (s : in_out[n.index].second)
	{
		if (std::find(scene.end() - std::min(scene.size(), 15ul), scene.end(), s) != scene.end()) continue;

		size_t const c = std::count(scene.begin(), scene.end(), s);

		if (c >= 1) continue;

		if (!scene.empty() && scene.back().seq == s.seq) continue;

		choices.insert({c, s});
	}

	foreach (c : choices)
	{
		Step const s = c.second;

		scene.push_back(s);

		if (dfsScene(g, in_out, to(g, s).node, size - 1, scene))
			return true;

		scene.pop_back();
	}

	return false;
}

/*
Scene randomScene(Graph const & g, SeqNum const start, size_t const size)
{
	Scene v{start};

	auto const m = nodes(g);

	std::deque<NodeNum> recent;

	NodeNum node = g.to(start).node;

	while (v.size() < size)
	{
		vector<pair<SeqNum, NodeNum>> choices;

		foreach (s : m.at(node).second)
		{
			auto const to = g.to(s).node;
			if (std::find(recent.begin(), recent.end(), to) == recent.end() || m.at(node).first.empty())
				choices.push_back({s, g.to(s).node});
		}

		if (choices.empty())
			foreach (s : m.at(node).first)
				choices.push_back({s, g.from(s).node});

		assert(!choices.empty());

		auto p = choices[rand()%choices.size()];

		v.push_back(p.first);
		recent.push_back(p.second);
		if (recent.size() > 10) recent.pop_front();

		node = p.second;
	}

	return v;
}
*/

Path randomScene(Graph const & g, NodeNum const start, size_t const size)
{
	Path s;

	if (!dfsScene(g, in_out(g), start, size, s))
		throw runtime_error("could not find sequence");

	set<SeqNum> ss;
	foreach (x : s) ss.insert(x.seq);
	std::cout << ss.size() << " unique sequences\n";

	return s;
}

/*
Scene randomScene(Graph const & g, NodeNum const start, size_t const size)
{
	auto o = out(g, start);
	if (o.empty()) throw runtime_error("cannot start at node without outgoing nodes");

	std::random_shuffle(o.begin(), o.end());
	return randomScene(g, o.front(), size);
}
*/

vector<Path> paths_through(Graph const & g, Step s, unsigned in_size, unsigned out_size)
{
	vector<Path> v;

	foreach (pre : in_paths(g, from(g, s).node, in_size))
	foreach (post : out_paths(g, to(g, s).node, out_size))
	{
		Path path = pre;
		path.push_back(s);
		append(path, post);
		v.push_back(path);
	}

	return v;
}

Frames demoFrames(Graph const & g, Step const s, unsigned const frames_per_pos)
{
	Frames f;

	auto scenes = paths_through(g, s, 1, 1);

	cout << "Playing " << scenes.size() << " scenes.\n";

	foreach (scene : scenes)
	{
		Frames z = frames(g, scene, frames_per_pos);

		Position const last = z.back().second.back();
		z.back().second.insert(z.back().second.end(), 70, last);

		Frames const x = smoothen(z);
		f.push_back({"      ", vector<Position>(70, x.front().second.front())});
		append(f, x);
	}

	return f;
}


int main(int const argc, char const * const * const argv)
{
	try
	{
		std::srand(std::time(nullptr));

		optional<Config> const config = config_from_args(argc, argv);
		if (!config) return 0;

		Graph const graph = loadGraph(config->db);

		Frames fr;

		if (config->demo)
		{
			if (auto step = step_by_desc(graph, *config->demo))
				fr = demoFrames(graph, *step, config->frames_per_pos);
			else
				throw runtime_error("no such transition");
		}
		else if (!config->script.empty())
			fr = smoothen(frames(graph, readScene(graph, config->script), config->frames_per_pos));
		else if (optional<NodeNum> start = node_by_desc(graph, config->start))
			fr = smoothen(frames(graph, randomScene(graph, *start, config->num_transitions), config->frames_per_pos));
//		else if (optional<SeqNum> start = seq_by_desc(graph, config->start))
//			fr = frames(graph, Scene{randomScene(graph, *start, config->num_transitions)}, config->frames_per_pos);
		else
			throw runtime_error("no such position/transition");

		if (!glfwInit()) error("could not initialize GLFW");

		GLFWwindow * const window = glfwCreateWindow(640, 480, "Jiu Jitsu Mapper", nullptr, nullptr);
		if (!window) error("could not create window");

		glfwMakeContextCurrent(window);
		glfwSwapInterval(1);

		Camera camera;
		Style style;
		style.background_color = white;
		camera.zoom(0.3);
		//camera.zoom(0.9);

		string const separator = "      ";

		for (auto i = fr.begin(); i != fr.end(); ++i)
		{
			double const textwidth = style.sequenceFont.Advance((i->first + separator).c_str(), -1);
			V2 textpos{10,20};

			string caption = i->first;
			for (auto j = i+1; j != i + std::min(fr.end() - i, 6l); ++j)
				caption += separator + j->first;

			foreach (pos : i->second)
			{
				glfwPollEvents();
				if (glfwWindowShouldClose(window)) return 0;

				camera.rotateHorizontal(-0.005);
				camera.setOffset(xz(between(pos[0][Core], pos[1][Core])));

				if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) camera.rotateVertical(-0.05);
				if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) camera.rotateVertical(0.05);
				if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) camera.rotateHorizontal(-0.03);
				if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) camera.rotateHorizontal(0.03);
				if (glfwGetKey(window, GLFW_KEY_HOME) == GLFW_PRESS) camera.zoom(-0.05);
				if (glfwGetKey(window, GLFW_KEY_END) == GLFW_PRESS) camera.zoom(0.05);

				int bottom = 0;
				int width, height;
				glfwGetFramebufferSize(window, &width, &height);

				if (config->dimensions)
				{
					width = config->dimensions->first;

					bottom = height - config->dimensions->second;
					height = config->dimensions->second;
				}

				renderWindow(
					// views:
					{ {0, 0, 1, 1, none, 80}
					//{ {0, 0, 1, 1, optional<unsigned>(0), 90}
			//		, {1-.3-.02, .02, .3, .3, optional<unsigned>(0), 90}
			//		, {.02, .02, .3, .3, optional<unsigned>(1), 60}
					},

					nullptr, // no viables
					graph, pos, camera,
					none, // no highlighted joint
					false, // not edit mode
					0, bottom,
					width, height, {0} /* todo */, style);

//				renderText(style.sequenceFont, textpos, caption, black);
				textpos.x -= textwidth / (i->second.size()-1);

				glfwSwapBuffers(window);
			}

		}

		sleep(2);

		glfwTerminate();
	}
	catch (std::exception const & e)
	{
		std::cerr << "error: " << e.what() << '\n';
		return 1;
	}
}
